// ============================================================================
//  tab5_camera.h - minimal capture driver for the Tab5 MIPI-CSI camera
//
//  Uses Espressif "esp_video" (V4L2 API, /dev/video0) when it is present in the
//  core. If the headers are not found at compile time, CAM_SUPPORTED = 0 and the
//  station falls back to simulation buttons (the rest of the project still works).
//
//  STATUS: EXPERIMENTAL - validate on your hardware. The esp_video structures
//  have changed between versions; if compilation fails here, compare with the
//  "capture_stream" example of esp-video-components or M5Stack's Tab5 camera
//  example, and adapt begin(). Everything else in the sketch is independent.
// ============================================================================
#pragma once
#include <Arduino.h>

#if USE_CAMERA && __has_include("esp_video_init.h") && __has_include("linux/videodev2.h")
  #define CAM_SUPPORTED 1
  #include "esp_video_init.h"
  #include "esp_video_device.h"
  #include "linux/videodev2.h"
  #include "driver/i2c_master.h"
  #include <fcntl.h>
  #include <sys/ioctl.h>
  #include <sys/mman.h>
  #include <unistd.h>
#else
  #define CAM_SUPPORTED 0
#endif

namespace cam {
bool ok = false;
uint32_t W = 0, H = 0;
String err = "";

#if CAM_SUPPORTED
static int fd = -1;
static uint8_t* buf[2] = {nullptr, nullptr};

bool begin() {
  static esp_video_init_csi_config_t csi[1];
  memset(csi, 0, sizeof(csi));
  // Re-use the I2C bus already opened by M5Unified if possible (new i2c_master driver)
  i2c_master_bus_handle_t bus = nullptr;
  if (i2c_master_get_bus_handle((i2c_port_num_t)CAM_I2C_PORT, &bus) == ESP_OK && bus) {
    csi[0].sccb_config.init_sccb = false;
    csi[0].sccb_config.i2c_handle = bus;
  } else {
    csi[0].sccb_config.init_sccb = true;
    csi[0].sccb_config.i2c_config.port = CAM_I2C_PORT;
    csi[0].sccb_config.i2c_config.scl_pin = (gpio_num_t)CAM_SCL;
    csi[0].sccb_config.i2c_config.sda_pin = (gpio_num_t)CAM_SDA;
  }
  csi[0].sccb_config.freq = 100000;
  csi[0].reset_pin = GPIO_NUM_NC;
  csi[0].pwdn_pin = GPIO_NUM_NC;

  esp_video_init_config_t vc;
  memset(&vc, 0, sizeof(vc));
  vc.csi = csi;
  if (esp_video_init(&vc) != ESP_OK) { err = "esp_video_init"; return false; }

  fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
  if (fd < 0) { err = "open /dev/video0"; return false; }

  struct v4l2_format f;
  memset(&f, 0, sizeof f);
  f.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd, VIDIOC_G_FMT, &f) != 0) { err = "VIDIOC_G_FMT"; return false; }
  f.fmt.pix.pixelformat = V4L2_PIX_FMT_RGB565;
  if (ioctl(fd, VIDIOC_S_FMT, &f) != 0) { err = "RGB565 refuse"; return false; }
  W = f.fmt.pix.width;
  H = f.fmt.pix.height;

  struct v4l2_requestbuffers rb;
  memset(&rb, 0, sizeof rb);
  rb.count = 2;
  rb.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  rb.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_REQBUFS, &rb) != 0) { err = "VIDIOC_REQBUFS"; return false; }

  for (int i = 0; i < 2; i++) {
    struct v4l2_buffer b;
    memset(&b, 0, sizeof b);
    b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    b.memory = V4L2_MEMORY_MMAP;
    b.index = i;
    if (ioctl(fd, VIDIOC_QUERYBUF, &b) != 0) { err = "VIDIOC_QUERYBUF"; return false; }
    buf[i] = (uint8_t*)mmap(NULL, b.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, b.m.offset);
    if (!buf[i]) { err = "mmap"; return false; }
    if (ioctl(fd, VIDIOC_QBUF, &b) != 0) { err = "VIDIOC_QBUF"; return false; }
  }
  int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  if (ioctl(fd, VIDIOC_STREAMON, &type) != 0) { err = "STREAMON"; return false; }
  ok = true;
  return true;
}

// Calls fn(const uint16_t* rgb565, W, H) with the next frame
template <typename F>
bool grab(F fn) {
  if (!ok) return false;
  struct v4l2_buffer b;
  memset(&b, 0, sizeof b);
  b.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
  b.memory = V4L2_MEMORY_MMAP;
  if (ioctl(fd, VIDIOC_DQBUF, &b) != 0) return false;
  fn((const uint16_t*)buf[b.index], W, H);
  ioctl(fd, VIDIOC_QBUF, &b);
  return true;
}
#else
bool begin() { err = "esp_video absent du core"; return false; }
template <typename F> bool grab(F) { return false; }
#endif
}  // namespace cam
