// Copyright 2022 Jonathan Bohren, Clyde McQueen
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <functional>
#include <iostream>
#include <stdlib.h>
#include <string>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <unistd.h>

extern "C" {
#include "gst/app/gstappsink.h"
#include "gst/gst.h"
}

#include "camera_info_manager/camera_info_manager.hpp"
#include "image_transport/image_transport.hpp"

#include "sensor_msgs/image_encodings.hpp"
#include "sensor_msgs/msg/camera_info.hpp"
#include "sensor_msgs/msg/compressed_image.hpp"
#include "sensor_msgs/msg/image.hpp"

#include "gscam/gscam.hpp"

namespace gscam {

GSCam::GSCam(const rclcpp::NodeOptions &options)
    : rclcpp::Node("gscam_publisher", options), gsconfig_(""), pipeline_(NULL),
      sink_(NULL), camera_info_manager_(this), stop_signal_(false) {
  if (!this->configure()) {
    RCLCPP_FATAL(get_logger(), "Failed to configure gscam!");
    rclcpp::shutdown();
    return;
  }
  if (!this->init_stream()) {
    RCLCPP_FATAL(get_logger(), "Failed to initialize gscam stream!");
    rclcpp::shutdown();
    return;
  }
  timer_ =
      this->create_wall_timer(std::chrono::milliseconds(grab_period_in_msec_),
                              std::bind(&GSCam::run, this));
}

GSCam::~GSCam() {
  stop_signal_ = true;
  timer_.reset();
  this->cleanup_stream();
}

bool GSCam::configure() {
  // Get gstreamer configuration
  // (either from environment variable or ROS param)
  bool gsconfig_rosparam_defined = false;
  char *gsconfig_env = NULL;

  const auto gsconfig_rosparam = declare_parameter("gscam_config", "");
  gsconfig_rosparam_defined = !gsconfig_rosparam.empty();
  gsconfig_env = getenv("GSCAM_CONFIG");

  if (!gsconfig_env && !gsconfig_rosparam_defined) {
    RCLCPP_FATAL(get_logger(),
                 "Problem getting GSCAM_CONFIG environment variable and "
                 "'gscam_config' rosparam is not set. This is needed to set up "
                 "a gstreamer pipeline.");
    return false;
  } else if (gsconfig_env && gsconfig_rosparam_defined) {
    RCLCPP_FATAL(get_logger(), "Both GSCAM_CONFIG environment variable and "
                               "'gscam_config' rosparam are set. "
                               "Please only define one.");
    return false;
  } else if (gsconfig_env) {
    gsconfig_ = gsconfig_env;
    RCLCPP_INFO_STREAM(get_logger(), "Using gstreamer config from env: \""
                                         << gsconfig_env << "\"");
  } else if (gsconfig_rosparam_defined) {
    gsconfig_ = gsconfig_rosparam;
    RCLCPP_INFO_STREAM(get_logger(), "Using gstreamer config from rosparam: \""
                                         << gsconfig_rosparam << "\"");
  }

  // Get additional gscam configuration
  sync_sink_ = declare_parameter("sync_sink", true);
  preroll_ = declare_parameter("preroll", false);
  use_gst_timestamps_ = declare_parameter("use_gst_timestamps", false);

  // Get the camera parameters file
  camera_info_url_ = declare_parameter("camera_info_url", "");
  camera_name_ = declare_parameter("camera_name", "");

  int framerate = declare_parameter("framerate", 30);
  if (framerate < 0) {
    return false;
  } else if (framerate == 0) {
    grab_period_in_msec_ = 1;
  } else {
    grab_period_in_msec_ =
        std::max(1UL, 1000UL / static_cast<uint64_t>(framerate));
  }

  // Get the image encoding
  image_encoding_ = declare_parameter(
      "image_encoding", std::string(sensor_msgs::image_encodings::RGB8));
  if (image_encoding_ != sensor_msgs::image_encodings::RGB8 &&
      image_encoding_ != sensor_msgs::image_encodings::MONO8 &&
      image_encoding_ != sensor_msgs::image_encodings::YUV422 &&
      image_encoding_ != "jpeg") {
    RCLCPP_FATAL_STREAM(get_logger(),
                        "Unsupported image encoding: " + image_encoding_);
  }

  camera_info_manager_.setCameraName(camera_name_);

  if (camera_info_manager_.validateURL(camera_info_url_)) {
    camera_info_manager_.loadCameraInfo(camera_info_url_);
    RCLCPP_INFO_STREAM(get_logger(),
                       "Loaded camera calibration from " << camera_info_url_);
  } else {
    RCLCPP_WARN_STREAM(get_logger(),
                       "Camera info at: "
                           << camera_info_url_
                           << " not found. Using an uncalibrated config.");
  }

  // Get TF Frame
  frame_id_ = declare_parameter("frame_id", "camera_frame");
  if (frame_id_ == "camera_frame") {
    RCLCPP_WARN_STREAM(get_logger(), "No camera frame_id set, using frame \""
                                         << frame_id_ << "\".");
  }

  use_sensor_data_qos_ = declare_parameter("use_sensor_data_qos", false);

  return true;
}

bool GSCam::init_stream() {
  if (!gst_is_initialized()) {
    // Initialize gstreamer pipeline
    RCLCPP_DEBUG_STREAM(get_logger(), "Initializing gstreamer...");
    gst_init(0, 0);
  }

  RCLCPP_DEBUG_STREAM(get_logger(),
                      "Gstreamer Version: " << gst_version_string());

  GError *error = 0; // Assignment to zero is a gst requirement

  pipeline_ = gst_parse_launch(gsconfig_.c_str(), &error);
  if (pipeline_ == NULL) {
    RCLCPP_FATAL_STREAM(get_logger(), error->message);
    return false;
  }

  // Create RGB sink
  sink_ = gst_element_factory_make("appsink", NULL);
  GstCaps *caps = gst_app_sink_get_caps(GST_APP_SINK(sink_));

  // http://gstreamer.freedesktop.org/data/doc/gstreamer/head/pwg/html/section-types-definitions.html
  if (image_encoding_ == sensor_msgs::image_encodings::RGB8) {
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB",
                               "max-buffers", G_TYPE_INT, 1, "drop",
                               G_TYPE_BOOLEAN, true, NULL);
  } else if (image_encoding_ == sensor_msgs::image_encodings::MONO8) {
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "GRAY8",
                               NULL);
  } else if (image_encoding_ == sensor_msgs::image_encodings::YUV422) {
    caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "UYVY",
                               NULL);
  } else if (image_encoding_ == "jpeg") {
    caps = gst_caps_new_simple("image/jpeg", NULL, NULL);
  }

  gst_app_sink_set_caps(GST_APP_SINK(sink_), caps);
  gst_caps_unref(caps);

  // Set whether the sink should sync
  // Sometimes setting this to true can cause a large number of frames to be
  // dropped
  gst_base_sink_set_sync(GST_BASE_SINK(sink_), (sync_sink_) ? TRUE : FALSE);

  if (GST_IS_PIPELINE(pipeline_)) {
    GstPad *outpad = gst_bin_find_unlinked_pad(GST_BIN(pipeline_), GST_PAD_SRC);
    g_assert(outpad);

    GstElement *outelement = gst_pad_get_parent_element(outpad);
    g_assert(outelement);
    gst_object_unref(outpad);

    if (!gst_bin_add(GST_BIN(pipeline_), sink_)) {
      RCLCPP_FATAL(get_logger(), "gst_bin_add() failed");
      gst_object_unref(outelement);
      gst_object_unref(pipeline_);
      return false;
    }

    if (!gst_element_link(outelement, sink_)) {
      RCLCPP_FATAL(get_logger(),
                   "GStreamer: cannot link outelement(\"%s\") -> sink\n",
                   gst_element_get_name(outelement));
      gst_object_unref(outelement);
      gst_object_unref(pipeline_);
      return false;
    }

    gst_object_unref(outelement);
  } else {
    GstElement *launchpipe = pipeline_;
    pipeline_ = gst_pipeline_new(NULL);
    g_assert(pipeline_);

    gst_object_unparent(GST_OBJECT(launchpipe));

    gst_bin_add_many(GST_BIN(pipeline_), launchpipe, sink_, NULL);

    if (!gst_element_link(launchpipe, sink_)) {
      RCLCPP_FATAL(get_logger(), "GStreamer: cannot link launchpipe -> sink");
      gst_object_unref(pipeline_);
      return false;
    }
  }

  // Calibration between ros::Time and gst timestamps
  GstClock *clock = gst_system_clock_obtain();
  GstClockTime ct = gst_clock_get_time(clock);
  gst_object_unref(clock);
  time_offset_ = now().nanoseconds() - GST_TIME_AS_NSECONDS(ct);
  RCLCPP_INFO(get_logger(), "Time offset: %.6f",
              rclcpp::Time(time_offset_).seconds());

  gst_element_set_state(pipeline_, GST_STATE_PAUSED);

  if (gst_element_get_state(pipeline_, NULL, NULL, -1) ==
      GST_STATE_CHANGE_FAILURE) {
    RCLCPP_FATAL(get_logger(),
                 "Failed to PAUSE stream, check your gstreamer configuration.");
    return false;
  } else {
    RCLCPP_DEBUG_STREAM(get_logger(), "Stream is PAUSED.");
  }

  // Create ROS camera interface
  const auto qos =
      use_sensor_data_qos_ ? rclcpp::SensorDataQoS() : rclcpp::QoS{1};
  if (image_encoding_ == "jpeg") {
    jpeg_pub_ = create_publisher<sensor_msgs::msg::CompressedImage>(
        "camera/image_raw/compressed", qos);
    cinfo_pub_ = create_publisher<sensor_msgs::msg::CameraInfo>(
        "camera/camera_info", qos);
  } else {
    camera_pub_ = image_transport::create_camera_publisher(
        this, "camera/image_raw", qos.get_rmw_qos_profile());
  }

  if (gst_element_set_state(pipeline_, GST_STATE_PLAYING) ==
      GST_STATE_CHANGE_FAILURE) {
    RCLCPP_ERROR(get_logger(), "Could not start stream!");
    return false;
  }
  RCLCPP_INFO(get_logger(), "Started stream.");

  return true;
}

void GSCam::publish_stream() {
  // This should block until a new frame is awake, this way, we'll run at the
  // actual capture framerate of the device.
  // RCLCPP_DEBUG(get_logger(), "Getting data...");
  GstSample *sample = gst_app_sink_pull_sample(GST_APP_SINK(sink_));
  if (!sample) {
    RCLCPP_ERROR(get_logger(), "Could not get gstreamer sample.");
    stop_signal_.store(true);
    return;
  }
  GstBuffer *buf = gst_sample_get_buffer(sample);
  GstMemory *memory = gst_buffer_get_memory(buf, 0);
  GstMapInfo info;

  gst_memory_map(memory, &info, GST_MAP_READ);
  gsize &buf_size = info.size;
  guint8 *&buf_data = info.data;

  GstClockTime bt = gst_element_get_base_time(pipeline_);

  // Stop on end of stream
  if (!buf) {
    RCLCPP_INFO(get_logger(), "Stream ended.");
    stop_signal_.store(true);
    return;
  }

  // Get the image width and height
  GstPad *pad = gst_element_get_static_pad(sink_, "sink");
  const GstCaps *caps = gst_pad_get_current_caps(pad);
  GstStructure *structure = gst_caps_get_structure(caps, 0);
  gst_structure_get_int(structure, "width", &width_);
  gst_structure_get_int(structure, "height", &height_);

  // Update header information
  sensor_msgs::msg::CameraInfo cur_cinfo = camera_info_manager_.getCameraInfo();
  sensor_msgs::msg::CameraInfo::SharedPtr cinfo;
  cinfo.reset(new sensor_msgs::msg::CameraInfo(cur_cinfo));
  if (use_gst_timestamps_) {
    cinfo->header.stamp =
        rclcpp::Time(GST_TIME_AS_NSECONDS(buf->pts + bt) + time_offset_);
  } else {
    cinfo->header.stamp = now();
  }

  cinfo->header.frame_id = frame_id_;
  if (image_encoding_ == "jpeg") {
    sensor_msgs::msg::CompressedImage::SharedPtr img(
        new sensor_msgs::msg::CompressedImage());
    img->header = cinfo->header;
    img->format = "jpeg";
    img->data.resize(buf_size);
    std::copy(buf_data, (buf_data) + (buf_size), img->data.begin());
    jpeg_pub_->publish(*img);
    cinfo_pub_->publish(*cinfo);
  } else {
    // Complain if the returned buffer is smaller than we expect
    const unsigned int expected_frame_size =
        width_ * height_ *
        sensor_msgs::image_encodings::numChannels(image_encoding_);

    if (buf_size < expected_frame_size) {
      RCLCPP_WARN_STREAM(
          get_logger(),
          "GStreamer image buffer underflow: Expected frame to be "
              << expected_frame_size << " bytes but got only " << buf_size
              << " bytes. (make sure frames are correctly encoded)");
    }

    // Construct Image message
    sensor_msgs::msg::Image::SharedPtr img(new sensor_msgs::msg::Image());

    img->header = cinfo->header;

    // Image data and metadata
    img->width = width_;
    img->height = height_;
    img->encoding = image_encoding_;
    img->is_bigendian = false;
    img->data.resize(expected_frame_size);

    // Copy only the data we received
    // Since we're publishing shared pointers, we need to copy the image so
    // we can free the buffer allocated by gstreamer
    img->step =
        width_ * sensor_msgs::image_encodings::numChannels(image_encoding_);

    std::copy(buf_data, (buf_data) + (buf_size), img->data.begin());

    // Publish the image/info
    camera_pub_.publish(img, cinfo);
  }

  // Release the buffer
  if (buf) {
    gst_memory_unmap(memory, &info);
    gst_memory_unref(memory);
    gst_sample_unref(sample);
  }
}

void GSCam::cleanup_stream() {
  // Clean up
  RCLCPP_INFO(get_logger(), "Stopping gstreamer pipeline...");
  if (pipeline_) {
    gst_element_set_state(pipeline_, GST_STATE_NULL);
    gst_object_unref(pipeline_);
    pipeline_ = NULL;
  }
}

void GSCam::run() {
  if (stop_signal_.load()) {
    rclcpp::shutdown();
    return;
  }
  this->publish_stream();
}

// Example callbacks for appsink
// TODO(someone): enable callback-based capture
void gst_eos_cb(GstAppSink *appsink, gpointer user_data) {}
GstFlowReturn gst_new_preroll_cb(GstAppSink *appsink, gpointer user_data) {
  return GST_FLOW_OK;
}
GstFlowReturn gst_new_asample_cb(GstAppSink *appsink, gpointer user_data) {
  return GST_FLOW_OK;
}

} // namespace gscam

#include "rclcpp_components/register_node_macro.hpp"

RCLCPP_COMPONENTS_REGISTER_NODE(gscam::GSCam)
