/*
    ROS driver to broadcast stereo images from a stereo webcam (eg. Minoru)
    This doesn't do any stsreo correspondence.  It merely broadcasts the images.
    Copyright (C) 2012 Bob Mottram
    fuzzgun@gmail.com

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <ros/ros.h>
#include <std_msgs/String.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/SetCameraInfo.h>
#include <image_transport/image_transport.h>
#include <pcl_ros/point_cloud.h>
#include <pcl/point_types.h>
#include <pcl_conversions/pcl_conversions.h>
#include <sensor_msgs/LaserScan.h>
#include <sstream>
#include <omp.h>

#include <iostream>
#include <stdio.h>

#include <time.h>


#include "v4l2stereo_no_sse/anyoption.h"
#include "v4l2stereo_no_sse/drawing.h"
#include "v4l2stereo_no_sse/stereo.h"
#include "v4l2stereo_no_sse/fast.h"
#include "v4l2stereo_no_sse/libcam.h"
#include "v4l2stereo_no_sse/camcalib.h"
#include "v4l2stereo_no_sse/pointcloud.h"
#include "v4l2stereo_no_sse/elas/elas.h"

#include <pthread.h>

pthread_t imgCaptureThread;
pthread_mutex_t imgCopyMutex;


using namespace std;
using namespace cv;

std::string dev_left = "";
std::string dev_right = "";
int ww = 320;
int hh = 240;
int fps = 30;
int exposure = 0;

Camera *left_camera = NULL;
Camera *right_camera = NULL;

IplImage *l=NULL;
IplImage *r=NULL;
unsigned char *l_=NULL;
unsigned char *r_=NULL;
unsigned char *buffer=NULL;
unsigned char *tmp_l=NULL;
unsigned char *tmp_r=NULL;

bool profiling=false;

bool flip_left_image=false;
bool flip_right_image=false;
bool histogram_equalization=false;
bool publish_raw_image=true;
bool publish_disparity=true;
bool publish_pointcloud=true;
bool publish_laserscan=true;

sensor_msgs::Image raw_image;
image_transport::Publisher image_pub;
image_transport::Publisher disp_pub;

ros::Publisher cloud_pub;
pcl::PointCloud<pcl::PointXYZRGB> cloud;
float cloud_range_max = 6000;

ros::Publisher scan_pub;
sensor_msgs::LaserScan scan;
float scan_angle_min = -1.57;
float scan_angle_max = 1.57;
float scan_angle_increment = 3.14 / 1000;
float scan_range_min = 0;
float scan_range_max = 6000;
bool  scan_range_def_infinity = true;
float scan_height_min = 100;
float scan_height_max = 150;

camcalib *camera_calibration=NULL;
std::string calibration_filename = "calibration.txt";
bool rectify_images = false;

uint8_t * I1 = NULL;
uint8_t * I2 = NULL;
float * left_disparities = NULL;
float * right_disparities = NULL;
Elas * elas = NULL;

IplImage* hist_image0 = NULL;
IplImage* hist_image1 = NULL;

IplImage * disparity_image = NULL;
IplImage * points_image = NULL;

ros::Time capture_time;

void elas_disparity_map(
  unsigned char * left_image,
  unsigned char * right_image,
  int image_width,
  int image_height,
  uint8_t * &I1,
  uint8_t * &I2,
  float * &left_disparities,
  float * &right_disparities,
  Elas * &elas)
{
  if (elas==NULL) {
    Elas::parameters param;
    elas = new Elas(param);
    I1 = new uint8_t[image_width*image_height];
    I2 = new uint8_t[image_width*image_height];
    left_disparities = new float[image_width*image_height];
    right_disparities = new float[image_width*image_height];
  }

  // convert to single byte format
  for (int i = 0; i < image_width*image_height; i++) {
    I1[i] = (uint8_t)left_image[i*3+2];
    I2[i] = (uint8_t)right_image[i*3+2];
  }

  const int32_t dims[3] = {image_width, image_height, image_width};
  elas->process(I1,I2,left_disparities,right_disparities,dims);
}


/*!
 * \brief stop the stereo camera
 * \param left_camera left camera object
 * \param right_camera right camera object
 */
void stop_cameras(
                  Camera *&left_camera,
                  Camera *&right_camera)
{
  if (left_camera != NULL) {
    delete left_camera;
    delete right_camera;
    left_camera = NULL;
    right_camera = NULL;
  }
}

void start_cameras(
                   Camera *&left_camera,
                   Camera *&right_camera,
                   std::string dev_left, std::string dev_right,
                   int width, int height,
                   int fps)
{
  if (left_camera != NULL) {
    stop_cameras(left_camera,right_camera);
  }

  l = cvCreateImage(cvSize(width, height), 8, 3);
  r = cvCreateImage(cvSize(width, height), 8, 3);

//  l_=(unsigned char *)l->imageData;
//  r_=(unsigned char *)r->imageData;

  left_camera = new Camera(dev_left.c_str(), width, height, fps);
  right_camera = new Camera(dev_right.c_str(), width, height, fps);

  if (exposure == 0) {
    left_camera->setExposureAuto();
    right_camera->setExposureAuto();
  } else {
    left_camera->setExposureAutoOff();
    right_camera->setExposureAutoOff();
    left_camera->setExposure(exposure);
    right_camera->setExposure(exposure);
  }

  camera_calibration = new camcalib();
  camera_calibration->ParseCalibrationFile(calibration_filename);
  rectify_images = camera_calibration->rectification_loaded;

  hist_image0 = cvCreateImage( cvGetSize(l), IPL_DEPTH_8U, 1 );
  hist_image1 = cvCreateImage( cvGetSize(l), IPL_DEPTH_8U, 1 );

  ros::NodeHandle n;

  raw_image.width  = width;
  raw_image.height = height;
  raw_image.step = width * 3;
  raw_image.encoding = "bgr8";
  raw_image.data.resize(width*height*3);

  if (publish_raw_image) {
    image_transport::ImageTransport it1(n);
    std::string topic_str = "left/image_raw";
    image_pub = it1.advertise(topic_str.c_str(), 1);
  }

  if (publish_disparity) {
    image_transport::ImageTransport it2(n);
    std::string topic_str = "disparity";
    disp_pub = it2.advertise(topic_str.c_str(), 1);
  }

  if (publish_pointcloud) {
    cloud_pub = n.advertise<pcl::PointCloud<pcl::PointXYZRGB>>("pointcloud", 1);
  }

  if (publish_laserscan) {
    scan_pub = n.advertise<sensor_msgs::LaserScan>("laserscan", 1);
  }
}

// flip the given image so that the camera can be mounted upside down
void flip(unsigned char* raw_image, unsigned char* flipped_frame_buf) {
  int max = ww * hh * 3;
  for (int i = 0; i < max; i += 3) {
    flipped_frame_buf[i] = raw_image[(max - 3 - i)];
    flipped_frame_buf[i + 1] = raw_image[(max - 3 - i + 1)];
    flipped_frame_buf[i + 2] = raw_image[(max - 3 - i + 2)];
  }
  memcpy(raw_image, flipped_frame_buf, max * sizeof(unsigned char));
}



void colorize_disparities( unsigned char * img,
                           float * left_disparities,
                           int ww,
                           int hh,
                           int min_disparity )
{
  for (int i = 0; i < ww*hh; i++) {
    if (left_disparities[i] > min_disparity) {
      float val = min(( *(((float*)left_disparities)+i) )*0.01f,1.0f);
      if (val <= 0) {
        img[3*i+0] = 0;
        img[3*i+1] = 0;
        img[3*i+2] = 0;
      } else {
        float h2 = 6.0f * (1.0f - val);
        unsigned char x  = (unsigned char)((1.0f - fabs(fmod(h2, 2.0f) - 1.0f))*255);
        if (0 <= h2&&h2<1) {
          img[3*i+0] = 255;
          img[3*i+1] = x;
          img[3*i+2] = 0;
        }
        else if (1<=h2&&h2<2)  {
          img[3*i+0] = x;
          img[3*i+1] = 255;
          img[3*i+2] = 0;
        }
        else if (2<=h2&&h2<3)  {
          img[3*i+0] = 0;
          img[3*i+1] = 255;
          img[3*i+2] = x;
        }
        else if (3<=h2&&h2<4)  {
          img[3*i+0] = 0;
          img[3*i+1] = x;
          img[3*i+2] = 255;
        }
        else if (4<=h2&&h2<5)  {
          img[3*i+0] = x;
          img[3*i+1] = 0;
          img[3*i+2] = 255;
        }
        else if (5<=h2&&h2<=6) {
          img[3*i+0] = 255;
          img[3*i+1] = 0;
          img[3*i+2] = x;
        }
      }
    }
    else {
      img[3*i+0] = 0;
      img[3*i+1] = 0;
      img[3*i+2] = 0;
    }
  }

}


void pointcloud_publish( unsigned char *image_buf,
                         IplImage *points_image,
                         CvMat *pose,
                         float baseline )
{
//  pcl_conversions::toPCL(ros::Time::now(), cloud.header.stamp);
  pcl_conversions::toPCL(capture_time, cloud.header.stamp);
  cloud.header.frame_id = "camera_frame";
  cloud.points.resize(0);
  cloud.is_dense = true;

  float pose_y = -(float)cvmGet(pose,0,3) / 1000;
  float pose_z = -(float)cvmGet(pose,1,3) / 1000;
  float pose_x = (float)cvmGet(pose,2,3) / 1000;

  float * points_image_data = (float*)points_image->imageData;
  int pixels = points_image->width * points_image->height;
  int n = 0, ctr = 0;
  pcl::PointXYZRGB point;
  for (int i = 0; i < pixels; i++, n += 3) {
    point.y = -points_image_data[n] / 1000;
    point.z = -points_image_data[n+1] / 1000;
    point.x = points_image_data[n+2] / 1000;

    float dx = point.x - pose_x;
    float dy = point.y - pose_y;
    float dz = point.z - pose_z;

    if ((fabs(dx)+fabs(dy)+fabs(dz) > 0.001) &&
        (fabs(dx) < cloud_range_max) &&
        (fabs(dy) < cloud_range_max) &&
        (fabs(dz) < cloud_range_max)) {
      point.b = image_buf[n];
      point.g = image_buf[n+1];
      point.r = image_buf[n+2];
      cloud.points.push_back(point);
      ctr++;
    }
  }
  if (ctr > 0) {
    cloud.height = 1;
    cloud.width = ctr;
    cloud_pub.publish(cloud);
  }
}



void laserscan_publish( unsigned char *image_buf,
                        IplImage *points_image,
                        CvMat *pose,
                        float baseline )
{
//  scan.header.stamp = ros::Time::now();
  scan.header.stamp = capture_time;
  scan.header.frame_id = "camera_frame";
  scan.angle_min = scan_angle_min;
  scan.angle_max = scan_angle_max;
  scan.angle_increment = scan_angle_increment;
  scan.time_increment = 0.0;
  scan.range_min = scan_range_min;
  scan.range_max = scan_range_max;

  //determine amount of rays to create
  uint32_t ranges_size = std::ceil((scan_angle_max - scan_angle_min) / scan_angle_increment);

  if (scan_range_def_infinity)
    scan.ranges.assign(ranges_size, std::numeric_limits<double>::infinity());
  else
    scan.ranges.assign(ranges_size, scan_range_max - 0.01);

  scan.intensities.resize(0);

  float pose_x = (float)cvmGet(pose,0,3) / 1000;
  float pose_y = (float)cvmGet(pose,1,3) / 1000;
  float pose_z = (float)cvmGet(pose,2,3) / 1000;

  float * points_image_data = (float*)points_image->imageData;
  int pixels = points_image->width * points_image->height;
  int n = 0, ctr = 0;
  for (int i = 0; i < pixels; i++, n += 3) {
    float x = points_image_data[n] / 1000;
    float y = points_image_data[n+1] / 1000;
    float z = points_image_data[n+2] / 1000;

    float dx = x - pose_x;
    float dy = y - pose_y;
    float dz = z - pose_z;

//    point.b = image_buf[n];
//    point.g = image_buf[n+1];
//    point.r = image_buf[n+2];

    if (y < scan_height_min || y > scan_height_max) {
      continue;
    }

    double range = hypot(x,z);
    if (range < scan_range_min) {
      continue;
    }

    // sign in front of atan2 controls the scan direction (pos = CW; neg = CCW)
    double angle = -atan2(x,z);
    if (angle < scan_angle_min || angle > scan_angle_max) {
      continue;
    }

    //overwrite range at laserscan ray if new range is smaller
    int index = (angle - scan_angle_min) / scan_angle_increment;
    if (range < scan.ranges[index]) {
      scan.ranges[index] = range;
    }

    ctr++;
  }

  if (ctr > 0) {
    scan_pub.publish(scan);
  }
}


void cleanup()
{
  pthread_cancel(imgCaptureThread);

  cvReleaseImage(&l);
  cvReleaseImage(&r);

  if (hist_image0 != NULL) cvReleaseImage(&hist_image0);
  if (hist_image1 != NULL) cvReleaseImage(&hist_image1);
  if (disparity_image != NULL) cvReleaseImage(&disparity_image);
  if (points_image != NULL) cvReleaseImage(&points_image);

  if (elas != NULL) delete elas;
  delete camera_calibration;

  if (buffer != NULL) delete[] buffer;

  if (tmp_l != NULL) delete[] tmp_l;
  if (tmp_r != NULL) delete[] tmp_r;

  if (l_ != NULL) delete[] l_;
  if (r_ != NULL) delete[] r_;

  stop_cameras(left_camera,right_camera);
}


unsigned long cap_cntr = 0;
float cap_avg_time = 0;

void *CaptureImages(void *threadid)
{
  clock_t cap_start;
  float cap_time;

  while(1)
  {
    if (profiling)
      cap_start = clock();

    // capture frames
    if (!left_camera->Update(right_camera, 25, 1000)) {
      pthread_exit(NULL);
    }

    // Convert to IplImage
    left_camera->toIplImage(l);
    right_camera->toIplImage(r);

    pthread_mutex_trylock(&imgCopyMutex);
    memcpy((void*)tmp_l, l->imageData, ww*hh*3);
    memcpy((void*)tmp_r, r->imageData, ww*hh*3);
    pthread_mutex_unlock(&imgCopyMutex);

    if (profiling) {
      cap_cntr++;
      cap_time = ((float)(clock() - cap_start))/CLOCKS_PER_SEC;
      cap_avg_time = cap_avg_time * 0.9 + cap_time * 0.1;
    }
  }

}


int main(int argc, char** argv)
{
  ros::init(argc, argv, "stereocamera");
  ros::NodeHandle nh("~");

  // set some default values
  int val=0;
  std::string str="";
  dev_left = "/dev/video1";
  dev_right = "/dev/video0";

  nh.getParam("profiling", profiling);

  nh.getParam("width", val);
  if (val>0) ww=val;
  nh.getParam("height", val);
  if (val>0) hh=val;
  nh.getParam("fps", val);
  if (val>0) fps=val;
  nh.getParam("dev_left", str);
  if (str!="") dev_left=str;
  nh.getParam("dev_right", str);
  if (str!="") dev_right=str;
  nh.getParam("flip_left", flip_left_image);
  nh.getParam("flip_right", flip_right_image);

  nh.getParam("exposure", exposure);

  nh.getParam("calibration_filename", str);
  if (str!="") calibration_filename=str;

  nh.getParam("hist_equal", histogram_equalization);

  nh.getParam("pub_image", publish_raw_image);

  nh.getParam("pub_disparity", publish_disparity);

  nh.getParam("pub_cloud", publish_pointcloud);
  nh.getParam("cloud_range_max", cloud_range_max);

  nh.getParam("pub_laser", publish_laserscan);
  nh.getParam("scan_angle_min", scan_angle_min);
  nh.getParam("scan_angle_max", scan_angle_max);
  nh.getParam("scan_angle_increment", scan_angle_increment);
  nh.getParam("scan_range_min", scan_range_min);
  nh.getParam("scan_range_max", scan_range_max);
  nh.getParam("scan_range_def_infinity", scan_range_def_infinity);
  nh.getParam("scan_height_min", scan_height_min);
  nh.getParam("scan_height_max", scan_height_max);


  start_cameras(left_camera,right_camera,
                dev_left, dev_right,
                ww, hh, fps);

  tmp_l = new unsigned char[ww * hh * 3];
  tmp_r = new unsigned char[ww * hh * 3];

  int s = pthread_create(&imgCaptureThread, NULL, &CaptureImages, NULL);
  if ( s != 0 ) {
    ROS_ERROR("Camera THREAD error");
    exit(1);
  }

  l_ = new unsigned char[ww * hh * 3];
  r_ = new unsigned char[ww * hh * 3];

  ros::NodeHandle n;
  bool publishing = false;

  clock_t start;

  while (n.ok()) {

    if (profiling)
      start = clock();

    capture_time = ros::Time::now();

    pthread_mutex_lock(&imgCopyMutex);
    memcpy((void*)l_, tmp_l, ww*hh*3);
    memcpy((void*)r_, tmp_r, ww*hh*3);
    pthread_mutex_unlock(&imgCopyMutex);

    // flip images
    if (flip_left_image) {
      if (buffer == NULL) {
        buffer = new unsigned char[ww * hh * 3];
      }
      flip(l_, buffer);
    }

    if (flip_right_image) {
      if (buffer == NULL) {
        buffer = new unsigned char[ww * hh * 3];
      }
      flip(r_, buffer);
    }

    if (publish_disparity || publish_pointcloud || publish_laserscan) {
      // rectify images
      if (!rectify_images) {
        ROS_ERROR("Images need to be recrified before using ELAS");
        break;
      }
#pragma omp parallel for
      for (int cam = 0; cam <= 1; cam++) {
        if (cam == 0) {
          camera_calibration->RectifyImage(0, ww, hh, l_, -camera_calibration->v_shift);
        }
        else {
          camera_calibration->RectifyImage(1, ww, hh, r_, +camera_calibration->v_shift);
        }
      }

      // histogram equalization
      if (histogram_equalization) {
        if (buffer == NULL) {
          buffer = new unsigned char[ww * hh * 3];
        }
        memcpy((void*)buffer, l_, ww*hh*3);

#pragma omp parallel for
        for (int i = 0; i < 2; i++) {
          unsigned char *img = l_;
          IplImage* hist_image = hist_image0;
          if (i > 0) {
            img = r_;
            hist_image = hist_image1;
          }
          svs::histogram_equalise( hist_image, img, ww, hh );
        }
      }

      //ros::spinOnce();

      //calculate disparities
      elas_disparity_map(l_, r_, ww, hh, I1, I2, left_disparities, right_disparities, elas);
      //ros::spinOnce();
    }

    if (publish_pointcloud || publish_laserscan) {
      // convert disparity map to 3D points
      pointcloud::disparity_map_to_3d_points(
          left_disparities, ww, hh,
          camera_calibration->disparityToDepth,
          camera_calibration->pose,
          disparity_image, points_image );
    }

    if (!publishing) {
      if (publish_raw_image)
        ROS_INFO("Publishing left raw image...");
      if (publish_disparity)
        ROS_INFO("Publishing disparity image...");
      if (publish_pointcloud)
        ROS_INFO("Publishing pointcloud...");
      if (publish_laserscan)
        ROS_INFO("Publishing laserscan...");
      publishing = true;
    }

    unsigned char *img;
    if ( histogram_equalization && (publish_disparity || publish_pointcloud || publish_laserscan) )
      img = buffer;
    else
      img = l_;

    // publish raw image
    if (publish_raw_image) {
      // Convert to sensor_msgs::Image
      memcpy ((void*)(&raw_image.data[0]), (void*)img, ww*hh*3);
      // publish image
      image_pub.publish(raw_image);
    }

    // publish disparity image
    if (publish_disparity) {
      // Color code image based on disparity
      colorize_disparities( img, left_disparities, ww, hh, 0 );
      memcpy ((void*)(&raw_image.data[0]), (void*)img, ww*hh*3);
      // publish image
      disp_pub.publish(raw_image);
    }

    // publish pointcloud
    if (publish_pointcloud) {
      pointcloud_publish( img,
                          points_image,
                          camera_calibration->pose,
                          cvmGet(camera_calibration->extrinsicTranslation,0,0) );
    }

    // publish laser scan
    if (publish_laserscan) {
      laserscan_publish( img,
                         points_image,
                         camera_calibration->pose,
                         cvmGet(camera_calibration->extrinsicTranslation,0,0) );
    }

    if (profiling) {
      ROS_INFO("TIME: total=%f capture=%f cap_cntr=%ld", ((float)(clock()-start))/CLOCKS_PER_SEC, cap_avg_time, cap_cntr);
    }

    ros::spinOnce();
  }

  cleanup();
  ROS_INFO("Stereo camera stopped");
}


