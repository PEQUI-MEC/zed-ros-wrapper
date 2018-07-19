///////////////////////////////////////////////////////////////////////////
//
// Copyright (c) 2018, STEREOLABS.
//
// All rights reserved.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
// "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
// LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
// A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
// OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
// SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
// LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
// DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
// THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
// (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
// OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
///////////////////////////////////////////////////////////////////////////
#include "zed_wrapper_nodelet.hpp"
#include "sl_tools.h"

#include <opencv2/imgproc/imgproc.hpp>

#ifndef NDEBUG
#include <ros/console.h>
#endif

#include <nav_msgs/Odometry.h>
#include <pcl_conversions/pcl_conversions.h>
#include <sensor_msgs/Imu.h>
#include <sensor_msgs/distortion_models.h>
#include <sensor_msgs/image_encodings.h>
#include <stereo_msgs/DisparityImage.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

// >>>>> Backward compatibility
#define COORDINATE_SYSTEM_IMAGE static_cast<sl::COORDINATE_SYSTEM>(0)
#define COORDINATE_SYSTEM_RIGHT_HANDED_Z_UP                                    \
    static_cast<sl::COORDINATE_SYSTEM>(3)
#define COORDINATE_SYSTEM_RIGHT_HANDED_Z_UP_X_FWD                              \
    static_cast<sl::COORDINATE_SYSTEM>(5)
// <<<<< Backward compatibility

using namespace std;

namespace zed_wrapper {

    ZEDWrapperNodelet::ZEDWrapperNodelet() : Nodelet() {}

    ZEDWrapperNodelet::~ZEDWrapperNodelet() {
        devicePollThread.get()->join();
    }

    void ZEDWrapperNodelet::onInit() {
#ifndef NDEBUG

        if (ros::console::set_logger_level(ROSCONSOLE_DEFAULT_NAME,
                                           ros::console::levels::Debug)) {
            ros::console::notifyLoggerLevelsChanged();
        }

#endif
        // Launch file parameters
        resolution = sl::RESOLUTION_HD720;
        quality = sl::DEPTH_MODE_PERFORMANCE;
        sensingMode = sl::SENSING_MODE_STANDARD;
        frameRate = 30;
        gpuId = -1;
        zedId = 0;
        mSerialNumber = 0;
        odometryDb = "";
        imuPubRate = 100.0;
        initialTrackPose.resize(6);
        mTerrainMap = true; // TODO set to false
        mTrackingReady = false;
        mMappingReady = false;
        mTerrainPubRate = 2.0;

        for (int i = 0; i < 6; i++) {
            initialTrackPose[i] = 0.0f;
        }

        mMatResizeFactor = 1.0;
        verbose = true;
        nh = getMTNodeHandle();
        nhNs = getMTPrivateNodeHandle();
        // Set  default coordinate frames
        nhNs.param<std::string>("pose_frame", mapFrameId, "pose_frame");
        nhNs.param<std::string>("odometry_frame", odometryFrameId, "odometry_frame");
        nhNs.param<std::string>("base_frame", baseFrameId, "base_frame");
        nhNs.param<std::string>("imu_frame", imuFrameId, "imu_link");
        nhNs.param<std::string>("left_camera_frame", leftCamFrameId,
                                "left_camera_frame");
        nhNs.param<std::string>("left_camera_optical_frame", leftCamOptFrameId,
                                "left_camera_optical_frame");
        nhNs.param<std::string>("right_camera_frame", rightCamFrameId,
                                "right_camera_frame");
        nhNs.param<std::string>("right_camera_optical_frame", rightCamOptFrameId,
                                "right_camera_optical_frame");
        depthFrameId = leftCamFrameId;
        depthOptFrameId = leftCamOptFrameId;
        // Note: Depth image frame id must match color image frame id
        cloudFrameId = depthOptFrameId;
        rgbFrameId = depthFrameId;
        rgbOptFrameId = cloudFrameId;
        disparityFrameId = depthFrameId;
        disparityOptFrameId = depthOptFrameId;
        confidenceFrameId = depthFrameId;
        confidenceOptFrameId = depthOptFrameId;
        // Get parameters from launch file
        nhNs.getParam("resolution", resolution);
        nhNs.getParam("frame_rate", frameRate);
        checkResolFps();
        nhNs.getParam("verbose", verbose);
        nhNs.getParam("quality", quality);
        nhNs.getParam("sensing_mode", sensingMode);
        nhNs.getParam("openni_depth_mode", mOpenniDepthMode);
        nhNs.getParam("gpu_id", gpuId);
        nhNs.getParam("zed_id", zedId);
        nhNs.getParam("depth_stabilization", mDepthStabilization);
        nhNs.getParam("terrain_mapping", mTerrainMap); // TODO Check SDK version
        int tmp_sn = 0;
        nhNs.getParam("serial_number", tmp_sn);

        if (tmp_sn > 0) {
            mSerialNumber = tmp_sn;
        }

        nhNs.getParam("camera_model", userCamModel);
        // Publish odometry tf
        nhNs.param<bool>("publish_tf", publishTf, true);

        if (mSerialNumber > 0) {
            NODELET_INFO_STREAM("SN : " << mSerialNumber);
        }

        // Print order frames
        NODELET_INFO_STREAM("pose_frame \t\t   -> " << mapFrameId);
        NODELET_INFO_STREAM("odometry_frame \t\t   -> " << odometryFrameId);
        NODELET_INFO_STREAM("base_frame \t\t   -> " << baseFrameId);
        NODELET_INFO_STREAM("imu_link \t\t   -> " << imuFrameId);
        NODELET_INFO_STREAM("left_camera_frame \t   -> " << leftCamFrameId);
        NODELET_INFO_STREAM("left_camera_optical_frame  -> " << leftCamOptFrameId);
        NODELET_INFO_STREAM("right_camera_frame \t   -> " << rightCamFrameId);
        NODELET_INFO_STREAM("right_camera_optical_frame -> " << rightCamOptFrameId);
        NODELET_INFO_STREAM("depth_frame \t\t   -> " << depthFrameId);
        NODELET_INFO_STREAM("depth_optical_frame \t   -> " << depthOptFrameId);
        NODELET_INFO_STREAM("disparity_frame \t   -> " << disparityFrameId);
        NODELET_INFO_STREAM("disparity_optical_frame    -> " << disparityOptFrameId);
        // Status of map TF
        NODELET_INFO_STREAM("Publish " << mapFrameId << " ["
                            << (publishTf ? "TRUE" : "FALSE") << "]");
        // Status of odometry TF
        // NODELET_INFO_STREAM("Publish " << odometry_frame_id << " [" << (publish_tf
        // ? "TRUE" : "FALSE") << "]");
        std::string img_topic = "image_rect_color";
        std::string img_raw_topic = "image_raw_color";
        // Set the default topic names
        string left_topic = "left/" + img_topic;
        string left_raw_topic = "left/" + img_raw_topic;
        string left_cam_info_topic = "left/camera_info";
        string left_cam_info_raw_topic = "left/camera_info_raw";
        string right_topic = "right/" + img_topic;
        string right_raw_topic = "right/" + img_raw_topic;
        string right_cam_info_topic = "right/camera_info";
        string right_cam_info_raw_topic = "right/camera_info_raw";
        string rgb_topic = "rgb/" + img_topic;
        string rgb_raw_topic = "rgb/" + img_raw_topic;
        string rgb_cam_info_topic = "rgb/camera_info";
        string rgb_cam_info_raw_topic = "rgb/camera_info_raw";
        string depth_topic = "depth/";

        if (mOpenniDepthMode) {
            NODELET_INFO_STREAM("Openni depth mode activated");
            depth_topic += "depth_raw_registered";
        } else {
            depth_topic += "depth_registered";
        }

        string depth_cam_info_topic = "depth/camera_info";
        string disparity_topic = "disparity/disparity_image";
        string point_cloud_topic = "point_cloud/cloud_registered";
        string conf_img_topic = "confidence/confidence_image";
        string conf_map_topic = "confidence/confidence_map";
        string pose_topic = "map";
        string odometry_topic = "odom";
        string imu_topic = "imu/data";
        string imu_topic_raw = "imu/data_raw";
        nhNs.getParam("rgb_topic", rgb_topic);
        nhNs.getParam("rgb_raw_topic", rgb_raw_topic);
        nhNs.getParam("rgb_cam_info_topic", rgb_cam_info_topic);
        nhNs.getParam("rgb_cam_info_raw_topic", rgb_cam_info_raw_topic);
        nhNs.getParam("left_topic", left_topic);
        nhNs.getParam("left_raw_topic", left_raw_topic);
        nhNs.getParam("left_cam_info_topic", left_cam_info_topic);
        nhNs.getParam("left_cam_info_raw_topic", left_cam_info_raw_topic);
        nhNs.getParam("right_topic", right_topic);
        nhNs.getParam("right_raw_topic", right_raw_topic);
        nhNs.getParam("right_cam_info_topic", right_cam_info_topic);
        nhNs.getParam("right_cam_info_raw_topic", right_cam_info_raw_topic);
        nhNs.getParam("depth_topic", depth_topic);
        nhNs.getParam("depth_cam_info_topic", depth_cam_info_topic);
        nhNs.getParam("disparity_topic", disparity_topic);
        nhNs.getParam("confidence_img_topic", conf_img_topic);
        nhNs.getParam("confidence_map_topic", conf_map_topic);
        nhNs.getParam("point_cloud_topic", point_cloud_topic);
        nhNs.getParam("pose_topic", pose_topic);
        nhNs.getParam("odometry_topic", odometry_topic);
        nhNs.getParam("imu_topic", imu_topic);
        nhNs.getParam("imu_topic_raw", imu_topic_raw);
        nhNs.getParam("imu_pub_rate", imuPubRate);
        // Create camera info
        sensor_msgs::CameraInfoPtr rgb_cam_info_ms_g(new sensor_msgs::CameraInfo());
        sensor_msgs::CameraInfoPtr left_cam_info_msg_(new sensor_msgs::CameraInfo());
        sensor_msgs::CameraInfoPtr right_cam_info_msg_(new sensor_msgs::CameraInfo());
        sensor_msgs::CameraInfoPtr rgb_cam_info_raw_msg_(
            new sensor_msgs::CameraInfo());
        sensor_msgs::CameraInfoPtr left_cam_info_raw_ms_g(
            new sensor_msgs::CameraInfo());
        sensor_msgs::CameraInfoPtr right_cam_info_raw_msg_(
            new sensor_msgs::CameraInfo());
        sensor_msgs::CameraInfoPtr depth_cam_info_msg_(new sensor_msgs::CameraInfo());
        mRgbCamInfoMsg = rgb_cam_info_ms_g;
        mLeftCamInfoMsg = left_cam_info_msg_;
        mRightCamInfoMsg = right_cam_info_msg_;
        mRgbCamInfoRawMsg = rgb_cam_info_raw_msg_;
        mLeftCamInfoRawMsg = left_cam_info_raw_ms_g;
        mRightCamInfoRawMsg = right_cam_info_raw_msg_;
        mDepthCamInfoMsg = depth_cam_info_msg_;
        // SVO
        nhNs.param<std::string>("svo_filepath", svoFilepath, std::string());
        // Initialize tf2 transformation
        nhNs.getParam("initial_tracking_pose", initialTrackPose);
        set_pose(initialTrackPose[0], initialTrackPose[1], initialTrackPose[2],
                 initialTrackPose[3], initialTrackPose[4], initialTrackPose[5]);
        // Initialization transformation listener
        tfBuffer.reset(new tf2_ros::Buffer);
        tfListener.reset(new tf2_ros::TransformListener(*tfBuffer));

        // Try to initialize the ZED
        if (!svoFilepath.empty()) {
            param.svo_input_filename = svoFilepath.c_str();
        } else {
            param.camera_fps = frameRate;
            param.camera_resolution = static_cast<sl::RESOLUTION>(resolution);

            if (mSerialNumber == 0) {
                param.camera_linux_id = zedId;
            } else {
                bool waiting_for_camera = true;

                while (waiting_for_camera) {
                    // Ctrl+C check
                    if (!nhNs.ok()) {
                        zed.close();
                        return;
                    }

                    sl::DeviceProperties prop = sl_tools::getZEDFromSN(mSerialNumber);

                    if (prop.id < -1 ||
                        prop.camera_state == sl::CAMERA_STATE::CAMERA_STATE_NOT_AVAILABLE) {
                        std::string msg = "ZED SN" + to_string(mSerialNumber) +
                                          " not detected ! Please connect this ZED";
                        NODELET_INFO_STREAM(msg.c_str());
                        std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                    } else {
                        waiting_for_camera = false;
                        param.camera_linux_id = prop.id;
                    }
                }
            }
        }

        std::string ver = sl_tools::getSDKVersion(verMajor, verMinor, verSubMinor);
        NODELET_INFO_STREAM("SDK version : " << ver);

        if (verMajor < 2) {
            NODELET_WARN_STREAM("Please consider to upgrade to latest SDK version to "
                                "get better performances");
            param.coordinate_system = COORDINATE_SYSTEM_IMAGE;
            NODELET_INFO_STREAM("Camera coordinate system : COORDINATE_SYSTEM_IMAGE");
            mIdxX = 2;
            mIdxY = 0;
            mIdxZ = 1;
            mSignX = 1;
            mSignY = -1;
            mSignZ = -1;
        } else if (verMajor == 2 && verMinor < 5) {
            NODELET_WARN_STREAM("Please consider to upgrade to latest SDK version to "
                                "get latest features");
            param.coordinate_system = COORDINATE_SYSTEM_RIGHT_HANDED_Z_UP;
            NODELET_INFO_STREAM(
                "Camera coordinate system : COORDINATE_SYSTEM_RIGHT_HANDED_Z_UP");
            mIdxX = 1;
            mIdxY = 0;
            mIdxZ = 2;
            mSignX = 1;
            mSignY = -1;
            mSignZ = 1;
        } else {
            param.coordinate_system = COORDINATE_SYSTEM_RIGHT_HANDED_Z_UP_X_FWD;
            NODELET_INFO_STREAM(
                "Camera coordinate system : COORDINATE_SYSTEM_RIGHT_HANDED_Z_UP_X_FWD");
            mIdxX = 0;
            mIdxY = 1;
            mIdxZ = 2;
            mSignX = 1;
            mSignY = 1;
            mSignZ = 1;
        }

        param.coordinate_units = sl::UNIT_METER;
        param.depth_mode = static_cast<sl::DEPTH_MODE>(quality);
        param.sdk_verbose = verbose;
        param.sdk_gpu_id = gpuId;
        param.depth_stabilization = mDepthStabilization;
        sl::ERROR_CODE err = sl::ERROR_CODE_CAMERA_NOT_DETECTED;

        while (err != sl::SUCCESS) {
            err = zed.open(param);
            NODELET_INFO_STREAM(toString(err));
            std::this_thread::sleep_for(std::chrono::milliseconds(2000));

            // Ctrl+C check
            if (!nhNs.ok()) {
                zed.close();
                return;
            }
        }

        realCamModel = zed.getCameraInformation().camera_model;
        std::string camModelStr = "LAST";

        if (realCamModel == sl::MODEL_ZED) {
            camModelStr = "ZED";

            if (userCamModel != 0) {
                NODELET_WARN("Camera model does not match user parameter. Please modify "
                             "the value of the parameter 'camera_model' to 0");
            }
        } else if (realCamModel == sl::MODEL_ZED_M) {
            camModelStr = "ZED M";

            if (userCamModel != 1) {
                NODELET_WARN("Camera model does not match user parameter. Please modify "
                             "the value of the parameter 'camera_model' to 1");
            }
        }

        NODELET_INFO_STREAM("CAMERA MODEL : " << realCamModel);
        mSerialNumber = zed.getCameraInformation().serial_number;
        // Dynamic Reconfigure parameters
        mDynRecServer =
            boost::make_shared<dynamic_reconfigure::Server<zed_wrapper::ZedConfig>>();
        dynamic_reconfigure::Server<zed_wrapper::ZedConfig>::CallbackType f;
        f = boost::bind(&ZEDWrapperNodelet::dynamicReconfCallback, this, _1, _2);
        mDynRecServer->setCallback(f);
        nhNs.getParam("mat_resize_factor", mMatResizeFactor);

        if (mMatResizeFactor < 0.1) {
            mMatResizeFactor = 0.1;
            NODELET_WARN_STREAM(
                "Minimum allowed values for 'mat_resize_factor' is 0.1");
        }

        if (mMatResizeFactor > 1.0) {
            mMatResizeFactor = 1.0;
            NODELET_WARN_STREAM(
                "Maximum allowed values for 'mat_resize_factor' is 1.0");
        }

        nhNs.getParam("confidence", mConfidence);
        nhNs.getParam("max_depth", mMaxDepth);
        nhNs.getParam("exposure", mExposure);
        nhNs.getParam("gain", mGain);
        nhNs.getParam("auto_exposure", mAutoExposure);

        if (mAutoExposure) {
            mTriggerAutoExposure = true;
        }

        // Create all the publishers
        // Image publishers
        image_transport::ImageTransport it_zed(nh);
        pubRgb = it_zed.advertise(rgb_topic, 1); // rgb
        NODELET_INFO_STREAM("Advertized on topic " << rgb_topic);
        pubRawRgb = it_zed.advertise(rgb_raw_topic, 1); // rgb raw
        NODELET_INFO_STREAM("Advertized on topic " << rgb_raw_topic);
        pubLeft = it_zed.advertise(left_topic, 1); // left
        NODELET_INFO_STREAM("Advertized on topic " << left_topic);
        pubRawLeft = it_zed.advertise(left_raw_topic, 1); // left raw
        NODELET_INFO_STREAM("Advertized on topic " << left_raw_topic);
        pubRight = it_zed.advertise(right_topic, 1); // right
        NODELET_INFO_STREAM("Advertized on topic " << right_topic);
        pubRawRight = it_zed.advertise(right_raw_topic, 1); // right raw
        NODELET_INFO_STREAM("Advertized on topic " << right_raw_topic);
        pubDepth = it_zed.advertise(depth_topic, 1); // depth
        NODELET_INFO_STREAM("Advertized on topic " << depth_topic);
        pubConfImg = it_zed.advertise(conf_img_topic, 1); // confidence image
        NODELET_INFO_STREAM("Advertized on topic " << conf_img_topic);
        // Confidence Map publisher
        pubConfMap =
            nh.advertise<sensor_msgs::Image>(conf_map_topic, 1); // confidence map
        NODELET_INFO_STREAM("Advertized on topic " << conf_map_topic);
        // Disparity publisher
        pubDisparity = nh.advertise<stereo_msgs::DisparityImage>(disparity_topic, 1);
        NODELET_INFO_STREAM("Advertized on topic " << disparity_topic);
        // PointCloud publisher
        pubCloud = nh.advertise<sensor_msgs::PointCloud2>(point_cloud_topic, 1);
        NODELET_INFO_STREAM("Advertized on topic " << point_cloud_topic);
        // Camera info publishers
        pubRgbCamInfo =
            nh.advertise<sensor_msgs::CameraInfo>(rgb_cam_info_topic, 1); // rgb
        NODELET_INFO_STREAM("Advertized on topic " << rgb_cam_info_topic);
        pubLeftCamInfo =
            nh.advertise<sensor_msgs::CameraInfo>(left_cam_info_topic, 1); // left
        NODELET_INFO_STREAM("Advertized on topic " << left_cam_info_topic);
        pubRightCamInfo =
            nh.advertise<sensor_msgs::CameraInfo>(right_cam_info_topic, 1); // right
        NODELET_INFO_STREAM("Advertized on topic " << right_cam_info_topic);
        pubDepthCamInfo =
            nh.advertise<sensor_msgs::CameraInfo>(depth_cam_info_topic, 1); // depth
        NODELET_INFO_STREAM("Advertized on topic " << depth_cam_info_topic);
        // Raw
        pubRgbCamInfoRaw = nh.advertise<sensor_msgs::CameraInfo>(
                               rgb_cam_info_raw_topic, 1); // raw rgb
        NODELET_INFO_STREAM("Advertized on topic " << rgb_cam_info_raw_topic);
        pubLeftCamInfoRaw = nh.advertise<sensor_msgs::CameraInfo>(
                                left_cam_info_raw_topic, 1); // raw left
        NODELET_INFO_STREAM("Advertized on topic " << left_cam_info_raw_topic);
        pubRightCamInfoRaw = nh.advertise<sensor_msgs::CameraInfo>(
                                 right_cam_info_raw_topic, 1); // raw right
        NODELET_INFO_STREAM("Advertized on topic " << right_cam_info_raw_topic);
        // Odometry and Map publisher
        pubPose = nh.advertise<geometry_msgs::PoseStamped>(pose_topic, 1);
        NODELET_INFO_STREAM("Advertized on topic " << pose_topic);
        pubOdom = nh.advertise<nav_msgs::Odometry>(odometry_topic, 1);
        NODELET_INFO_STREAM("Advertized on topic " << odometry_topic);

        // Imu publisher
        if (imuPubRate > 0 && realCamModel == sl::MODEL_ZED_M) {
            pubImu = nh.advertise<sensor_msgs::Imu>(imu_topic, 500);
            NODELET_INFO_STREAM("Advertized on topic " << imu_topic << " @ "
                                << imuPubRate << " Hz");
            pubImuRaw = nh.advertise<sensor_msgs::Imu>(imu_topic_raw, 500);
            NODELET_INFO_STREAM("Advertized on topic " << imu_topic_raw << " @ "
                                << imuPubRate << " Hz");
            imuTime = ros::Time::now();
            pubImuTimer = nhNs.createTimer(ros::Duration(1.0 / imuPubRate),
                                           &ZEDWrapperNodelet::imuPubCallback, this);
        } else if (imuPubRate > 0 && realCamModel == sl::MODEL_ZED) {
            NODELET_WARN_STREAM(
                "'imu_pub_rate' set to "
                << imuPubRate << " Hz"
                << " but ZED camera model does not support IMU data publishing.");
        }

        // Services
        mSrvSetInitPose = nh.advertiseService("set_initial_pose",
                                              &ZEDWrapperNodelet::on_set_pose, this);
        mSrvResetOdometry = nh.advertiseService(
                                "reset_odometry", &ZEDWrapperNodelet::on_reset_odometry, this);
        mSrvResetTracking = nh.advertiseService(
                                "reset_tracking", &ZEDWrapperNodelet::on_reset_tracking, this);
        // Start pool thread
        devicePollThread = boost::shared_ptr<boost::thread>(
                               new boost::thread(boost::bind(&ZEDWrapperNodelet::device_poll, this)));
    }

    void ZEDWrapperNodelet::checkResolFps() {
        switch (resolution) {
        case sl::RESOLUTION_HD2K:
            if (frameRate != 15) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution HD2K. Set to 15 FPS.");
                frameRate = 15;
            }

            break;

        case sl::RESOLUTION_HD1080:
            if (frameRate == 15 || frameRate == 30) {
                break;
            }

            if (frameRate > 15 && frameRate < 30) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution HD1080. Set to 15 FPS.");
                frameRate = 15;
            } else if (frameRate > 30) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution HD1080. Set to 30 FPS.");
                frameRate = 30;
            } else {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution HD1080. Set to 15 FPS.");
                frameRate = 15;
            }

            break;

        case sl::RESOLUTION_HD720:
            if (frameRate == 15 || frameRate == 30 || frameRate == 60) {
                break;
            }

            if (frameRate > 15 && frameRate < 30) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution HD720. Set to 15 FPS.");
                frameRate = 15;
            } else if (frameRate > 30 && frameRate < 60) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution HD720. Set to 30 FPS.");
                frameRate = 30;
            } else if (frameRate > 60) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution HD720. Set to 60 FPS.");
                frameRate = 60;
            } else {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution HD720. Set to 15 FPS.");
                frameRate = 15;
            }

            break;

        case sl::RESOLUTION_VGA:
            if (frameRate == 15 || frameRate == 30 || frameRate == 60 ||
                frameRate == 100) {
                break;
            }

            if (frameRate > 15 && frameRate < 30) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution VGA. Set to 15 FPS.");
                frameRate = 15;
            } else if (frameRate > 30 && frameRate < 60) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution VGA. Set to 30 FPS.");
                frameRate = 30;
            } else if (frameRate > 60 && frameRate < 100) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution VGA. Set to 60 FPS.");
                frameRate = 60;
            } else if (frameRate > 100) {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution VGA. Set to 100 FPS.");
                frameRate = 100;
            } else {
                NODELET_WARN_STREAM("Wrong FrameRate ("
                                    << frameRate
                                    << ") for the resolution VGA. Set to 15 FPS.");
                frameRate = 15;
            }

            break;

        default:
            NODELET_WARN_STREAM("Invalid resolution. Set to HD720 @ 30 FPS");
            resolution = 2;
            frameRate = 30;
        }
    }

    sensor_msgs::ImagePtr
    ZEDWrapperNodelet::imageToROSmsg(cv::Mat img, const std::string encodingType,
                                     std::string frameId, ros::Time t) {
        sensor_msgs::ImagePtr ptr = boost::make_shared<sensor_msgs::Image>();
        sensor_msgs::Image& imgMessage = *ptr;
        imgMessage.header.stamp = t;
        imgMessage.header.frame_id = frameId;
        imgMessage.height = img.rows;
        imgMessage.width = img.cols;
        imgMessage.encoding = encodingType;
        int num = 1; // for endianness detection
        imgMessage.is_bigendian = !(*(char*)&num == 1);
        imgMessage.step = img.cols * img.elemSize();
        size_t size = imgMessage.step * img.rows;
        imgMessage.data.resize(size);

        if (img.isContinuous()) {
            memcpy((char*)(&imgMessage.data[0]), img.data, size);
        } else {
            uchar* opencvData = img.data;
            uchar* rosData = (uchar*)(&imgMessage.data[0]);
            #pragma omp parallel for

            for (unsigned int i = 0; i < img.rows; i++) {
                memcpy(rosData, opencvData, imgMessage.step);
                rosData += imgMessage.step;
                opencvData += img.step;
            }
        }

        return ptr;
    }

    void ZEDWrapperNodelet::set_pose(float xt, float yt, float zt, float rr,
                                     float pr, float yr) {
        // ROS pose
        tf2::Quaternion q;
        q.setRPY(rr, pr, yr);
        tf2::Vector3 orig(xt, yt, zt);
        baseToOdomTransform.setOrigin(orig);
        baseToOdomTransform.setRotation(q);
        odomToMapTransform.setIdentity();
        // SL pose
        sl::float4 q_vec;
        q_vec[0] = q.x();
        q_vec[1] = q.y();
        q_vec[2] = q.z();
        q_vec[3] = q.w();
        sl::Orientation r(q_vec);
        initialPoseSl.setTranslation(sl::Translation(xt, yt, zt));
        initialPoseSl.setOrientation(r);
    }

    bool ZEDWrapperNodelet::on_set_pose(
        zed_wrapper::set_initial_pose::Request& req,
        zed_wrapper::set_initial_pose::Response& res) {
        initialTrackPose.resize(6);
        initialTrackPose[0] = req.x;
        initialTrackPose[1] = req.y;
        initialTrackPose[2] = req.z;
        initialTrackPose[3] = req.R;
        initialTrackPose[4] = req.P;
        initialTrackPose[5] = req.Y;
        set_pose(initialTrackPose[0], initialTrackPose[1], initialTrackPose[2],
                 initialTrackPose[3], initialTrackPose[4], initialTrackPose[5]);

        if (mTrackingActivated) {
            zed.resetTracking(initialPoseSl);
        }

        res.done = true;
        return true;
    }

    bool ZEDWrapperNodelet::on_reset_tracking(
        zed_wrapper::reset_tracking::Request& req,
        zed_wrapper::reset_tracking::Response& res) {
        if (!mTrackingActivated) {
            res.reset_done = false;
            return false;
        }

        nhNs.getParam("initial_tracking_pose", initialTrackPose);

        if (initialTrackPose.size() != 6) {
            NODELET_WARN_STREAM("Invalid Initial Pose size (" << initialTrackPose.size()
                                << "). Using Identity");
            initialPoseSl.setIdentity();
            odomToMapTransform.setIdentity();
            baseToOdomTransform.setIdentity();
        } else {
            set_pose(initialTrackPose[0], initialTrackPose[1], initialTrackPose[2],
                     initialTrackPose[3], initialTrackPose[4], initialTrackPose[5]);
        }

        zed.resetTracking(initialPoseSl);
        return true;
    }

    bool ZEDWrapperNodelet::on_reset_odometry(
        zed_wrapper::reset_odometry::Request& req,
        zed_wrapper::reset_odometry::Response& res) {
        initOdomWithPose = true;
        res.reset_done = true;
        return true;
    }

    void ZEDWrapperNodelet::start_tracking() {
        NODELET_INFO_STREAM("Starting Tracking");
        nhNs.getParam("odometry_DB", odometryDb);
        nhNs.getParam("pose_smoothing", poseSmoothing);
        nhNs.getParam("spatial_memory", spatialMemory);
        nhNs.getParam("floor_alignment", mFloorAlignment);

        if (mTerrainMap && !mFloorAlignment) {
            NODELET_INFO_STREAM("Floor Alignment required by Terrain Mapping algorithm");
            mFloorAlignment = true;
        }

        if (realCamModel == sl::MODEL_ZED_M) {
            nhNs.getParam("init_odom_with_imu", initOdomWithPose);
            NODELET_INFO_STREAM(
                "Init Odometry with first IMU data : " << initOdomWithPose);
        } else {
            initOdomWithPose = false;
        }

        if (initialTrackPose.size() != 6) {
            NODELET_WARN_STREAM("Invalid Initial Pose size (" << initialTrackPose.size()
                                << "). Using Identity");
            initialPoseSl.setIdentity();
            odomToMapTransform.setIdentity();
            odomToMapTransform.setIdentity();
        } else {
            set_pose(initialTrackPose[0], initialTrackPose[1], initialTrackPose[2],
                     initialTrackPose[3], initialTrackPose[4], initialTrackPose[5]);
        }

        if (odometryDb != "" && !sl_tools::file_exist(odometryDb)) {
            odometryDb = "";
            NODELET_WARN("odometry_DB path doesn't exist or is unreachable.");
        }

        // Tracking parameters
        sl::TrackingParameters trackParams;
        trackParams.area_file_path = odometryDb.c_str();
        trackParams.enable_pose_smoothing = poseSmoothing;
        NODELET_INFO_STREAM("Pose Smoothing : " << trackParams.enable_pose_smoothing);
        trackParams.enable_spatial_memory = spatialMemory;
        NODELET_INFO_STREAM("Spatial Memory : " << trackParams.enable_spatial_memory);
        trackParams.enable_floor_alignment = mFloorAlignment;
        NODELET_INFO_STREAM("Floor Alignment : " << trackParams.enable_floor_alignment);
        trackParams.initial_world_transform = initialPoseSl;
        zed.enableTracking(trackParams);
        mTrackingActivated = true;
        NODELET_INFO("Tracking ENABLED");

        if (mTerrainMap) { // TODO Check SDK version
            start_mapping();
        }
    }

    void ZEDWrapperNodelet::start_mapping() {
        if (!mTerrainMap) {
            return;
        }

        nhNs.getParam("terrain_pub_rate", mTerrainPubRate);
        sl::TerrainMappingParameters terrainParams;
        float agent_step = 0.05; // TODO Expose parameter to launch file
        float agent_slope = 20/*degrees*/; // TODO Expose parameter to launch file
        float agent_radius = 0.18; // TODO Expose parameter to launch file
        float agent_height = 0.8; // TODO Expose parameter to launch file
        float agent_roughness = 0.05; // TODO Expose parameter to launch file
        terrainParams.setAgentParameters(sl::UNIT_METER, agent_step, agent_slope, agent_radius,
                                         agent_height, agent_roughness);
        float height_thresh = .5f; // TODO Expose parameter to launch file
        float height_resol = .025f; // TODO Expose parameter to launch file
        sl::TerrainMappingParameters::GRID_RESOLUTION grid_resolution = sl::TerrainMappingParameters::GRID_RESOLUTION::MEDIUM; // TODO Expose parameter to launch file
        NODELET_INFO_STREAM("Grid Resolution " << terrainParams.setGridResolution(grid_resolution) << "M");
        NODELET_INFO_STREAM("Cutting height " << terrainParams.setHeightThreshold(sl::UNIT_METER, height_thresh) << "m");
        NODELET_INFO_STREAM("Z Resolution " << terrainParams.setZResolution(sl::UNIT_METER, height_resol) << "m");
        terrainParams.enable_traversability_cost_computation = true; // TODO Expose parameter to launch file
        terrainParams.enable_dynamic_extraction = true; // TODO Expose parameter to launch file
        terrainParams.enable_color_extraction = true; // TODO Expose parameter to launch file

        if (zed.enableTerrainMapping(terrainParams) == sl::SUCCESS) {
            // Start Terrain Mapping Timer
            mTerrainTimer = nhNs.createTimer(ros::Duration(1.0 / mTerrainPubRate),
                                             &ZEDWrapperNodelet::terrainCallback, this);
            NODELET_INFO_STREAM("Terrain Mapping: ENABLED @ " << mTerrainPubRate << "Hz");
            mMappingReady = true;
        } else {
            NODELET_WARN_STREAM("Terrain Mapping: NOT ENABLED");
        }
    }

    void ZEDWrapperNodelet::publishOdom(tf2::Transform odom_base_transform,
                                        ros::Time t) {
        nav_msgs::Odometry odom;
        odom.header.stamp = t;
        odom.header.frame_id = odometryFrameId; // odom_frame
        odom.child_frame_id = baseFrameId;      // base_frame
        // conversion from Tranform to message
        geometry_msgs::Transform base2 = tf2::toMsg(odom_base_transform);
        // Add all value in odometry message
        odom.pose.pose.position.x = base2.translation.x;
        odom.pose.pose.position.y = base2.translation.y;
        odom.pose.pose.position.z = base2.translation.z;
        odom.pose.pose.orientation.x = base2.rotation.x;
        odom.pose.pose.orientation.y = base2.rotation.y;
        odom.pose.pose.orientation.z = base2.rotation.z;
        odom.pose.pose.orientation.w = base2.rotation.w;
        // Publish odometry message
        pubOdom.publish(odom);
    }

    void ZEDWrapperNodelet::publishOdomFrame(tf2::Transform baseTransform,
            ros::Time t) {
        geometry_msgs::TransformStamped transformStamped;
        transformStamped.header.stamp = t;
        transformStamped.header.frame_id = odometryFrameId;
        transformStamped.child_frame_id = baseFrameId;
        // conversion from Tranform to message
        transformStamped.transform = tf2::toMsg(baseTransform);
        // Publish transformation
        transformOdomBroadcaster.sendTransform(transformStamped);
    }

    void ZEDWrapperNodelet::publishPose(tf2::Transform poseBaseTransform,
                                        ros::Time t) {
        geometry_msgs::PoseStamped pose;
        pose.header.stamp = t;
        pose.header.frame_id = mapFrameId; // map_frame
        // conversion from Tranform to message
        geometry_msgs::Transform base2 = tf2::toMsg(poseBaseTransform);
        // Add all value in Pose message
        pose.pose.position.x = base2.translation.x;
        pose.pose.position.y = base2.translation.y;
        pose.pose.position.z = base2.translation.z;
        pose.pose.orientation.x = base2.rotation.x;
        pose.pose.orientation.y = base2.rotation.y;
        pose.pose.orientation.z = base2.rotation.z;
        pose.pose.orientation.w = base2.rotation.w;
        // Publish odometry message
        pubPose.publish(pose);
    }

    void ZEDWrapperNodelet::publishPoseFrame(tf2::Transform baseTransform,
            ros::Time t) {
        geometry_msgs::TransformStamped transformStamped;
        transformStamped.header.stamp = t;
        transformStamped.header.frame_id = mapFrameId;
        transformStamped.child_frame_id = odometryFrameId;
        // conversion from Tranform to message
        transformStamped.transform = tf2::toMsg(baseTransform);
        // Publish transformation
        transformPoseBroadcaster.sendTransform(transformStamped);
    }

    void ZEDWrapperNodelet::publishImuFrame(tf2::Transform baseTransform) {
        geometry_msgs::TransformStamped transformStamped;
        transformStamped.header.stamp = imuTime;
        transformStamped.header.frame_id = baseFrameId;
        transformStamped.child_frame_id = imuFrameId;
        // conversion from Tranform to message
        transformStamped.transform = tf2::toMsg(baseTransform);
        // Publish transformation
        transformImuBroadcaster.sendTransform(transformStamped);
    }

    void ZEDWrapperNodelet::publishImage(cv::Mat img,
                                         image_transport::Publisher& pubImg,
                                         string imgFrameId, ros::Time t) {
        pubImg.publish(
            imageToROSmsg(img, sensor_msgs::image_encodings::BGR8, imgFrameId, t));
    }

    void ZEDWrapperNodelet::publishDepth(cv::Mat depth, ros::Time t) {
        string encoding;

        if (mOpenniDepthMode) {
            depth *= 1000.0f;
            depth.convertTo(depth, CV_16UC1); // in mm, rounded
            encoding = sensor_msgs::image_encodings::TYPE_16UC1;
        } else {
            encoding = sensor_msgs::image_encodings::TYPE_32FC1;
        }

        pubDepth.publish(imageToROSmsg(depth, encoding, depthOptFrameId, t));
    }

    void ZEDWrapperNodelet::publishDisparity(cv::Mat disparity, ros::Time t) {
        sl::CameraInformation zedParam =
            zed.getCameraInformation(sl::Resolution(matWidth, matHeight));
        sensor_msgs::ImagePtr disparity_image = imageToROSmsg(
                disparity, sensor_msgs::image_encodings::TYPE_32FC1, disparityFrameId, t);
        stereo_msgs::DisparityImage msg;
        msg.image = *disparity_image;
        msg.header = msg.image.header;
        msg.f = zedParam.calibration_parameters.left_cam.fx;
        msg.T = zedParam.calibration_parameters.T.x;
        msg.min_disparity = msg.f * msg.T / zed.getDepthMaxRangeValue();
        msg.max_disparity = msg.f * msg.T / zed.getDepthMinRangeValue();
        pubDisparity.publish(msg);
    }

    void ZEDWrapperNodelet::publishPointCloud(int width, int height) {
        pcl::PointCloud<pcl::PointXYZRGB> point_cloud;
        point_cloud.width = width;
        point_cloud.height = height;
        int size = width * height;
        point_cloud.points.resize(size);
        sl::Vector4<float>* cpu_cloud = mCloud.getPtr<sl::float4>();
        #pragma omp parallel for

        for (int i = 0; i < size; i++) {
            // COORDINATE_SYSTEM_RIGHT_HANDED_Z_UP_X_FWD
            point_cloud.points[i].x = mSignX * cpu_cloud[i][mIdxX];
            point_cloud.points[i].y = mSignY * cpu_cloud[i][mIdxY];
            point_cloud.points[i].z = mSignZ * cpu_cloud[i][mIdxZ];
            point_cloud.points[i].rgb = cpu_cloud[i][3];
        }

        sensor_msgs::PointCloud2 output;
        pcl::toROSMsg(point_cloud,
                      output); // Convert the point cloud to a ROS message
        output.header.frame_id =
            mPointCloudFrameId; // Set the header values of the ROS message
        output.header.stamp = mPointCloudTime;
        output.height = height;
        output.width = width;
        output.is_bigendian = false;
        output.is_dense = false;
        pubCloud.publish(output);
    }

    void ZEDWrapperNodelet::publishCamInfo(sensor_msgs::CameraInfoPtr camInfoMsg,
                                           ros::Publisher pubCamInfo, ros::Time t) {
        static int seq = 0;
        camInfoMsg->header.stamp = t;
        camInfoMsg->header.seq = seq;
        pubCamInfo.publish(camInfoMsg);
        seq++;
    }

    void ZEDWrapperNodelet::fillCamInfo(
        sl::Camera& zed, sensor_msgs::CameraInfoPtr left_cam_info_msg,
        sensor_msgs::CameraInfoPtr right_cam_info_msg, string leftFrameId,
        string rightFrameId, bool rawParam /*= false*/) {
        sl::CalibrationParameters zedParam;

        if (rawParam)
            zedParam = zed.getCameraInformation(sl::Resolution(matWidth, matHeight))
                       .calibration_parameters_raw;
        else
            zedParam = zed.getCameraInformation(sl::Resolution(matWidth, matHeight))
                       .calibration_parameters;

        float baseline = zedParam.T[0];
        left_cam_info_msg->distortion_model =
            sensor_msgs::distortion_models::PLUMB_BOB;
        right_cam_info_msg->distortion_model =
            sensor_msgs::distortion_models::PLUMB_BOB;
        left_cam_info_msg->D.resize(5);
        right_cam_info_msg->D.resize(5);
        left_cam_info_msg->D[0] = zedParam.left_cam.disto[0];   // k1
        left_cam_info_msg->D[1] = zedParam.left_cam.disto[1];   // k2
        left_cam_info_msg->D[2] = zedParam.left_cam.disto[4];   // k3
        left_cam_info_msg->D[3] = zedParam.left_cam.disto[2];   // p1
        left_cam_info_msg->D[4] = zedParam.left_cam.disto[3];   // p2
        right_cam_info_msg->D[0] = zedParam.right_cam.disto[0]; // k1
        right_cam_info_msg->D[1] = zedParam.right_cam.disto[1]; // k2
        right_cam_info_msg->D[2] = zedParam.right_cam.disto[4]; // k3
        right_cam_info_msg->D[3] = zedParam.right_cam.disto[2]; // p1
        right_cam_info_msg->D[4] = zedParam.right_cam.disto[3]; // p2
        left_cam_info_msg->K.fill(0.0);
        right_cam_info_msg->K.fill(0.0);
        left_cam_info_msg->K[0] = zedParam.left_cam.fx;
        left_cam_info_msg->K[2] = zedParam.left_cam.cx;
        left_cam_info_msg->K[4] = zedParam.left_cam.fy;
        left_cam_info_msg->K[5] = zedParam.left_cam.cy;
        left_cam_info_msg->K[8] = 1.0;
        right_cam_info_msg->K[0] = zedParam.right_cam.fx;
        right_cam_info_msg->K[2] = zedParam.right_cam.cx;
        right_cam_info_msg->K[4] = zedParam.right_cam.fy;
        right_cam_info_msg->K[5] = zedParam.right_cam.cy;
        right_cam_info_msg->K[8] = 1.0;
        left_cam_info_msg->R.fill(0.0);
        right_cam_info_msg->R.fill(0.0);

        for (int i = 0; i < 3; i++) {
            // identity
            right_cam_info_msg->R[i + i * 3] = 1;
            left_cam_info_msg->R[i + i * 3] = 1;
        }

        if (rawParam) {
            cv::Mat R_ = sl_tools::convertRodrigues(zedParam.R);
            float* p = (float*)R_.data;

            for (int i = 0; i < 9; i++) {
                right_cam_info_msg->R[i] = p[i];
            }
        }

        left_cam_info_msg->P.fill(0.0);
        right_cam_info_msg->P.fill(0.0);
        left_cam_info_msg->P[0] = zedParam.left_cam.fx;
        left_cam_info_msg->P[2] = zedParam.left_cam.cx;
        left_cam_info_msg->P[5] = zedParam.left_cam.fy;
        left_cam_info_msg->P[6] = zedParam.left_cam.cy;
        left_cam_info_msg->P[10] = 1.0;
        // http://docs.ros.org/api/sensor_msgs/html/msg/CameraInfo.html
        right_cam_info_msg->P[3] = (-1 * zedParam.left_cam.fx * baseline);
        right_cam_info_msg->P[0] = zedParam.right_cam.fx;
        right_cam_info_msg->P[2] = zedParam.right_cam.cx;
        right_cam_info_msg->P[5] = zedParam.right_cam.fy;
        right_cam_info_msg->P[6] = zedParam.right_cam.cy;
        right_cam_info_msg->P[10] = 1.0;
        left_cam_info_msg->width = right_cam_info_msg->width = matWidth;
        left_cam_info_msg->height = right_cam_info_msg->height = matHeight;
        left_cam_info_msg->header.frame_id = leftFrameId;
        right_cam_info_msg->header.frame_id = rightFrameId;
    }

    void ZEDWrapperNodelet::dynamicReconfCallback(zed_wrapper::ZedConfig& config,
            uint32_t level) {
        switch (level) {
        case 0:
            mConfidence = config.confidence;
            NODELET_INFO("Reconfigure confidence : %d", mConfidence);
            break;

        case 1:
            mExposure = config.exposure;
            NODELET_INFO("Reconfigure exposure : %d", mExposure);
            break;

        case 2:
            mGain = config.gain;
            NODELET_INFO("Reconfigure gain : %d", mGain);
            break;

        case 3:
            mAutoExposure = config.auto_exposure;

            if (mAutoExposure) {
                mTriggerAutoExposure = true;
            }

            NODELET_INFO("Reconfigure auto control of exposure and gain : %s",
                         mAutoExposure ? "Enable" : "Disable");
            break;

        case 4:
            mMatResizeFactor = config.mat_resize_factor;
            NODELET_INFO("Reconfigure mat_resize_factor: %g", mMatResizeFactor);
            mDataMutex.lock();
            matWidth = static_cast<int>(camWidth * mMatResizeFactor);
            matHeight = static_cast<int>(camHeight * mMatResizeFactor);
            NODELET_DEBUG_STREAM("Data Mat size : " << matWidth << "x" << matHeight);
            // Update Camera Info
            fillCamInfo(zed, mLeftCamInfoMsg, mRightCamInfoMsg, leftCamOptFrameId,
                        rightCamOptFrameId);
            fillCamInfo(zed, mLeftCamInfoRawMsg, mRightCamInfoRawMsg, leftCamOptFrameId,
                        rightCamOptFrameId, true);
            mRgbCamInfoMsg = mDepthCamInfoMsg = mLeftCamInfoMsg; // the reference camera is
            // the Left one (next to
            // the ZED logo)
            mRgbCamInfoRawMsg = mLeftCamInfoRawMsg;
            mDataMutex.unlock();
            break;

        case 5:
            mMaxDepth = config.max_depth;
            NODELET_INFO("Reconfigure max depth : %g", mMaxDepth);
            break;
        }
    }

    void ZEDWrapperNodelet::terrainCallback(const ros::TimerEvent& e) {
        if (!mTrackingActivated) {
            NODELET_DEBUG("Tracking not yet active");
            return;
        }

        if (!mMappingReady) {
            start_mapping();
        }

        // Request Terrain calculation
        zed.requestTerrainAsync();

        if (zed.getTerrainRequestStatusAsync() == sl::SUCCESS) {
            sl::Terrain terrain;

            if (zed.retrieveTerrainAsync(terrain) == sl::SUCCESS) {
                NODELET_DEBUG("Terrain available");
            }
        }
    }

    void ZEDWrapperNodelet::imuPubCallback(const ros::TimerEvent& e) {
        int imu_SubNumber = pubImu.getNumSubscribers();
        int imu_RawSubNumber = pubImuRaw.getNumSubscribers();

        if (imu_SubNumber < 1 && imu_RawSubNumber < 1) {
            return;
        }

        ros::Time t =
            sl_tools::slTime2Ros(zed.getTimestamp(sl::TIME_REFERENCE_IMAGE));
        sl::IMUData imu_data;
        zed.getIMUData(imu_data, sl::TIME_REFERENCE_CURRENT);

        if (imu_SubNumber > 0) {
            sensor_msgs::Imu imu_msg;
            imu_msg.header.stamp = imuTime; // t;
            imu_msg.header.frame_id = imuFrameId;
            imu_msg.orientation.x = mSignX * imu_data.getOrientation()[mIdxX];
            imu_msg.orientation.y = mSignY * imu_data.getOrientation()[mIdxY];
            imu_msg.orientation.z = mSignZ * imu_data.getOrientation()[mIdxZ];
            imu_msg.orientation.w = imu_data.getOrientation()[3];
            imu_msg.angular_velocity.x = mSignX * imu_data.angular_velocity[mIdxX];
            imu_msg.angular_velocity.y = mSignY * imu_data.angular_velocity[mIdxY];
            imu_msg.angular_velocity.z = mSignZ * imu_data.angular_velocity[mIdxZ];
            imu_msg.linear_acceleration.x = mSignX * imu_data.linear_acceleration[mIdxX];
            imu_msg.linear_acceleration.y = mSignY * imu_data.linear_acceleration[mIdxY];
            imu_msg.linear_acceleration.z = mSignZ * imu_data.linear_acceleration[mIdxZ];

            for (int i = 0; i < 3; i += 3) {
                imu_msg.orientation_covariance[i * 3 + 0] =
                    imu_data.orientation_covariance.r[i * 3 + mIdxX];
                imu_msg.orientation_covariance[i * 3 + 1] =
                    imu_data.orientation_covariance.r[i * 3 + mIdxY];
                imu_msg.orientation_covariance[i * 3 + 2] =
                    imu_data.orientation_covariance.r[i * 3 + mIdxZ];
                imu_msg.linear_acceleration_covariance[i * 3 + 0] =
                    imu_data.linear_acceleration_convariance.r[i * 3 + mIdxX];
                imu_msg.linear_acceleration_covariance[i * 3 + 1] =
                    imu_data.linear_acceleration_convariance.r[i * 3 + mIdxY];
                imu_msg.linear_acceleration_covariance[i * 3 + 2] =
                    imu_data.linear_acceleration_convariance.r[i * 3 + mIdxZ];
                imu_msg.angular_velocity_covariance[i * 3 + 0] =
                    imu_data.angular_velocity_convariance.r[i * 3 + mIdxX];
                imu_msg.angular_velocity_covariance[i * 3 + 1] =
                    imu_data.angular_velocity_convariance.r[i * 3 + mIdxY];
                imu_msg.angular_velocity_covariance[i * 3 + 2] =
                    imu_data.angular_velocity_convariance.r[i * 3 + mIdxZ];
            }

            pubImu.publish(imu_msg);
        }

        if (imu_RawSubNumber > 0) {
            sensor_msgs::Imu imu_raw_msg;
            imu_raw_msg.header.stamp = imuTime; // t;
            imu_raw_msg.header.frame_id = imuFrameId;
            imu_raw_msg.angular_velocity.x = mSignX * imu_data.angular_velocity[mIdxX];
            imu_raw_msg.angular_velocity.y = mSignY * imu_data.angular_velocity[mIdxY];
            imu_raw_msg.angular_velocity.z = mSignZ * imu_data.angular_velocity[mIdxZ];
            imu_raw_msg.linear_acceleration.x =
                mSignX * imu_data.linear_acceleration[mIdxX];
            imu_raw_msg.linear_acceleration.y =
                mSignY * imu_data.linear_acceleration[mIdxY];
            imu_raw_msg.linear_acceleration.z =
                mSignZ * imu_data.linear_acceleration[mIdxZ];

            for (int i = 0; i < 3; i += 3) {
                imu_raw_msg.linear_acceleration_covariance[i * 3 + 0] =
                    imu_data.linear_acceleration_convariance.r[i * 3 + mIdxX];
                imu_raw_msg.linear_acceleration_covariance[i * 3 + 1] =
                    imu_data.linear_acceleration_convariance.r[i * 3 + mIdxY];
                imu_raw_msg.linear_acceleration_covariance[i * 3 + 2] =
                    imu_data.linear_acceleration_convariance.r[i * 3 + mIdxZ];
                imu_raw_msg.angular_velocity_covariance[i * 3 + 0] =
                    imu_data.angular_velocity_convariance.r[i * 3 + mIdxX];
                imu_raw_msg.angular_velocity_covariance[i * 3 + 1] =
                    imu_data.angular_velocity_convariance.r[i * 3 + mIdxY];
                imu_raw_msg.angular_velocity_covariance[i * 3 + 2] =
                    imu_data.angular_velocity_convariance.r[i * 3 + mIdxZ];
            }

            imu_raw_msg.orientation_covariance[0] =
                -1; // Orientation data is not available in "data_raw" -> See ROS REP145
            // http://www.ros.org/reps/rep-0145.html#topics
            pubImuRaw.publish(imu_raw_msg);
        }

        // Publish IMU tf only if enabled
        if (publishTf) {
            // Camera to map transform from TF buffer
            tf2::Transform base_to_map;

            // Look up the transformation from base frame to map link
            try {
                // Save the transformation from base to frame
                geometry_msgs::TransformStamped b2m =
                    tfBuffer->lookupTransform(mapFrameId, baseFrameId, ros::Time(0));
                // Get the TF2 transformation
                tf2::fromMsg(b2m.transform, base_to_map);
            } catch (tf2::TransformException& ex) {
                NODELET_WARN_THROTTLE(
                    10.0, "The tf from '%s' to '%s' does not seem to be available. "
                    "IMU TF not published!",
                    baseFrameId.c_str(), mapFrameId.c_str());
                NODELET_DEBUG_THROTTLE(1.0, "Transform error: %s", ex.what());
                return;
            }

            // IMU Quaternion in Map frame
            tf2::Quaternion imu_q;
            imu_q.setX(mSignX * imu_data.getOrientation()[mIdxX]);
            imu_q.setY(mSignY * imu_data.getOrientation()[mIdxY]);
            imu_q.setZ(mSignZ * imu_data.getOrientation()[mIdxZ]);
            imu_q.setW(imu_data.getOrientation()[3]);
            // Pose Quaternion from ZED Camera
            tf2::Quaternion map_q = base_to_map.getRotation();
            // Difference between IMU and ZED Quaternion
            tf2::Quaternion delta_q = imu_q * map_q.inverse();
            tf2::Transform imu_pose;
            imu_pose.setIdentity();
            imu_pose.setRotation(delta_q);
            // Note, the frame is published, but its values will only change if someone
            // has subscribed to IMU
            publishImuFrame(imu_pose); // publish the imu Frame
        }
    }

    void ZEDWrapperNodelet::device_poll() {
        ros::Rate loop_rate(frameRate);
        ros::Time old_t =
            sl_tools::slTime2Ros(zed.getTimestamp(sl::TIME_REFERENCE_CURRENT));
        imuTime = old_t;
        sl::ERROR_CODE grab_status;
        mTrackingActivated = false;
        // Get the parameters of the ZED images
        camWidth = zed.getResolution().width;
        camHeight = zed.getResolution().height;
        NODELET_DEBUG_STREAM("Camera Frame size : " << camWidth << "x" << camHeight);
        matWidth = static_cast<int>(camWidth * mMatResizeFactor);
        matHeight = static_cast<int>(camHeight * mMatResizeFactor);
        NODELET_DEBUG_STREAM("Data Mat size : " << matWidth << "x" << matHeight);
        cv::Size cvSize(matWidth, matWidth);
        leftImRGB = cv::Mat(cvSize, CV_8UC3);
        rightImRGB = cv::Mat(cvSize, CV_8UC3);
        confImRGB = cv::Mat(cvSize, CV_8UC3);
        confMapFloat = cv::Mat(cvSize, CV_32FC1);
        // Create and fill the camera information messages
        fillCamInfo(zed, mLeftCamInfoMsg, mRightCamInfoMsg, leftCamOptFrameId,
                    rightCamOptFrameId);
        fillCamInfo(zed, mLeftCamInfoRawMsg, mRightCamInfoRawMsg, leftCamOptFrameId,
                    rightCamOptFrameId, true);
        mRgbCamInfoMsg = mDepthCamInfoMsg = mLeftCamInfoMsg; // the reference camera is
        // the Left one (next to the
        // ZED logo)
        mRgbCamInfoRawMsg = mLeftCamInfoRawMsg;
        sl::RuntimeParameters runParams;
        runParams.sensing_mode = static_cast<sl::SENSING_MODE>(sensingMode);
        sl::Mat leftZEDMat, rightZEDMat, depthZEDMat, disparityZEDMat, confImgZEDMat,
        confMapZEDMat;

        // Main loop
        while (nhNs.ok()) {
            // Check for subscribers
            int rgbSubnumber = pubRgb.getNumSubscribers();
            int rgbRawSubnumber = pubRawRgb.getNumSubscribers();
            int leftSubnumber = pubLeft.getNumSubscribers();
            int leftRawSubnumber = pubRawLeft.getNumSubscribers();
            int rightSubnumber = pubRight.getNumSubscribers();
            int rightRawSubnumber = pubRawRight.getNumSubscribers();
            int depthSubnumber = pubDepth.getNumSubscribers();
            int disparitySubnumber = pubDisparity.getNumSubscribers();
            int cloudSubnumber = pubCloud.getNumSubscribers();
            int poseSubnumber = pubPose.getNumSubscribers();
            int odomSubnumber = pubOdom.getNumSubscribers();
            int confImgSubnumber = pubConfImg.getNumSubscribers();
            int confMapSubnumber = pubConfMap.getNumSubscribers();
            int imuSubnumber = pubImu.getNumSubscribers();
            int imuRawsubnumber = pubImuRaw.getNumSubscribers();
            bool runLoop = mTerrainMap ||
                           ((rgbSubnumber + rgbRawSubnumber + leftSubnumber +
                             leftRawSubnumber + rightSubnumber + rightRawSubnumber +
                             depthSubnumber + disparitySubnumber + cloudSubnumber +
                             poseSubnumber + odomSubnumber + confImgSubnumber +
                             confMapSubnumber + imuSubnumber + imuRawsubnumber) > 0);
            runParams.enable_point_cloud = false;

            if (cloudSubnumber > 0) {
                runParams.enable_point_cloud = true;
            }

            // Run the loop only if there is some subscribers
            if (runLoop) {
                if ((mTerrainMap || mDepthStabilization || poseSubnumber > 0 || odomSubnumber > 0 ||
                     cloudSubnumber > 0 || depthSubnumber > 0) &&
                    !mTrackingActivated) { // Start the tracking
                    start_tracking();
                } else if (!mTerrainMap && !mDepthStabilization && poseSubnumber == 0 &&
                           odomSubnumber == 0 &&
                           mTrackingActivated) { // Stop the tracking
                    zed.disableTracking();
                    mTrackingActivated = false;
                }

                // Detect if one of the subscriber need to have the depth information
                mComputeDepth = mTerrainMap ||
                                ((depthSubnumber + disparitySubnumber + cloudSubnumber +
                                  poseSubnumber + odomSubnumber + confImgSubnumber +
                                  confMapSubnumber) > 0);
                // Timestamp
                ros::Time t =
                    sl_tools::slTime2Ros(zed.getTimestamp(sl::TIME_REFERENCE_IMAGE));
                mGrabbing = true;

                if (mComputeDepth) {
                    int actual_confidence = zed.getConfidenceThreshold();

                    if (actual_confidence != mConfidence) {
                        zed.setConfidenceThreshold(mConfidence);
                    }

                    float actual_max_depth = zed.getDepthMaxRangeValue();

                    if (actual_max_depth != mMaxDepth) {
                        zed.setDepthMaxRangeValue(mMaxDepth);
                    }

                    runParams.enable_depth = true; // Ask to compute the depth
                } else {
                    runParams.enable_depth = false;
                }

                grab_status = zed.grab(runParams); // Ask to not compute the depth
                mGrabbing = false;

                // cout << toString(grab_status) << endl;
                if (grab_status != sl::ERROR_CODE::SUCCESS) {
                    // Detect if a error occurred (for example:
                    // the zed have been disconnected) and
                    // re-initialize the ZED
                    if (grab_status != sl::ERROR_CODE_NOT_A_NEW_FRAME) {
                        NODELET_INFO_STREAM_ONCE(toString(grab_status));
                    }

                    std::this_thread::sleep_for(std::chrono::milliseconds(2));

                    if ((t - old_t).toSec() > 5) {
                        zed.close();
                        NODELET_INFO("Re-opening the ZED");
                        sl::ERROR_CODE err = sl::ERROR_CODE_CAMERA_NOT_DETECTED;

                        while (err != sl::SUCCESS) {
                            // Ctrl+C check
                            if (!nhNs.ok()) {
                                zed.close();
                                return;
                            }

                            int id = sl_tools::checkCameraReady(mSerialNumber);

                            if (id > 0) {
                                param.camera_linux_id = id;
                                err = zed.open(param); // Try to initialize the ZED
                                NODELET_INFO_STREAM(toString(err));
                            } else {
                                NODELET_INFO_STREAM("Waiting for the ZED (S/N " << mSerialNumber << ") to be re-connected");
                            }

                            std::this_thread::sleep_for(std::chrono::milliseconds(2000));
                        }

                        mTrackingActivated = false;

                        if (mTerrainMap || mDepthStabilization ||
                            poseSubnumber > 0 || odomSubnumber > 0) { // Start the tracking
                            start_tracking();
                        }
                    }

                    continue;
                }

                // Time update
                old_t = sl_tools::slTime2Ros(zed.getTimestamp(sl::TIME_REFERENCE_CURRENT));

                if (mAutoExposure) {
                    // getCameraSettings() can't check status of auto exposure
                    // triggerAutoExposure is used to execute setCameraSettings() only once
                    if (mTriggerAutoExposure) {
                        zed.setCameraSettings(sl::CAMERA_SETTINGS_EXPOSURE, 0, true);
                        mTriggerAutoExposure = false;
                    }
                } else {
                    int actual_exposure =
                        zed.getCameraSettings(sl::CAMERA_SETTINGS_EXPOSURE);

                    if (actual_exposure != mExposure) {
                        zed.setCameraSettings(sl::CAMERA_SETTINGS_EXPOSURE, mExposure);
                    }

                    int actual_gain = zed.getCameraSettings(sl::CAMERA_SETTINGS_GAIN);

                    if (actual_gain != mGain) {
                        zed.setCameraSettings(sl::CAMERA_SETTINGS_GAIN, mGain);
                    }
                }

                mDataMutex.lock();

                // Publish the left == rgb image if someone has subscribed to
                if (leftSubnumber > 0 || rgbSubnumber > 0) {
                    // Retrieve RGBA Left image
                    zed.retrieveImage(leftZEDMat, sl::VIEW_LEFT, sl::MEM_CPU, matWidth,
                                      matHeight);
                    cv::cvtColor(sl_tools::toCVMat(leftZEDMat), leftImRGB, CV_RGBA2RGB);

                    if (leftSubnumber > 0) {
                        publishCamInfo(mLeftCamInfoMsg, pubLeftCamInfo, t);
                        publishImage(leftImRGB, pubLeft, leftCamOptFrameId, t);
                    }

                    if (rgbSubnumber > 0) {
                        publishCamInfo(mRgbCamInfoMsg, pubRgbCamInfo, t);
                        publishImage(leftImRGB, pubRgb, depthOptFrameId,
                                     t); // rgb is the left image
                    }
                }

                // Publish the left_raw == rgb_raw image if someone has subscribed to
                if (leftRawSubnumber > 0 || rgbRawSubnumber > 0) {
                    // Retrieve RGBA Left image
                    zed.retrieveImage(leftZEDMat, sl::VIEW_LEFT_UNRECTIFIED, sl::MEM_CPU,
                                      matWidth, matHeight);
                    cv::cvtColor(sl_tools::toCVMat(leftZEDMat), leftImRGB, CV_RGBA2RGB);

                    if (leftRawSubnumber > 0) {
                        publishCamInfo(mLeftCamInfoRawMsg, pubLeftCamInfoRaw, t);
                        publishImage(leftImRGB, pubRawLeft, leftCamOptFrameId, t);
                    }

                    if (rgbRawSubnumber > 0) {
                        publishCamInfo(mRgbCamInfoRawMsg, pubRgbCamInfoRaw, t);
                        publishImage(leftImRGB, pubRawRgb, depthOptFrameId, t);
                    }
                }

                // Publish the right image if someone has subscribed to
                if (rightSubnumber > 0) {
                    // Retrieve RGBA Right image
                    zed.retrieveImage(rightZEDMat, sl::VIEW_RIGHT, sl::MEM_CPU, matWidth,
                                      matHeight);
                    cv::cvtColor(sl_tools::toCVMat(rightZEDMat), rightImRGB, CV_RGBA2RGB);
                    publishCamInfo(mRightCamInfoMsg, pubRightCamInfo, t);
                    publishImage(rightImRGB, pubRight, rightCamOptFrameId, t);
                }

                // Publish the right image if someone has subscribed to
                if (rightRawSubnumber > 0) {
                    // Retrieve RGBA Right image
                    zed.retrieveImage(rightZEDMat, sl::VIEW_RIGHT_UNRECTIFIED, sl::MEM_CPU,
                                      matWidth, matHeight);
                    cv::cvtColor(sl_tools::toCVMat(rightZEDMat), rightImRGB, CV_RGBA2RGB);
                    publishCamInfo(mRightCamInfoRawMsg, pubRightCamInfoRaw, t);
                    publishImage(rightImRGB, pubRawRight, rightCamOptFrameId, t);
                }

                // Publish the depth image if someone has subscribed to
                if (depthSubnumber > 0 || disparitySubnumber > 0) {
                    zed.retrieveMeasure(depthZEDMat, sl::MEASURE_DEPTH, sl::MEM_CPU,
                                        matWidth, matHeight);
                    publishCamInfo(mDepthCamInfoMsg, pubDepthCamInfo, t);
                    publishDepth(sl_tools::toCVMat(depthZEDMat), t); // in meters
                }

                // Publish the disparity image if someone has subscribed to
                if (disparitySubnumber > 0) {
                    zed.retrieveMeasure(disparityZEDMat, sl::MEASURE_DISPARITY, sl::MEM_CPU,
                                        matWidth, matHeight);
                    // Need to flip sign, but cause of this is not sure
                    cv::Mat disparity = sl_tools::toCVMat(disparityZEDMat) * -1.0;
                    publishDisparity(disparity, t);
                }

                // Publish the confidence image if someone has subscribed to
                if (confImgSubnumber > 0) {
                    zed.retrieveImage(confImgZEDMat, sl::VIEW_CONFIDENCE, sl::MEM_CPU,
                                      matWidth, matHeight);
                    cv::cvtColor(sl_tools::toCVMat(confImgZEDMat), confImRGB, CV_RGBA2RGB);
                    publishImage(confImRGB, pubConfImg, confidenceOptFrameId, t);
                }

                // Publish the confidence map if someone has subscribed to
                if (confMapSubnumber > 0) {
                    zed.retrieveMeasure(confMapZEDMat, sl::MEASURE_CONFIDENCE, sl::MEM_CPU,
                                        matWidth, matHeight);
                    confMapFloat = sl_tools::toCVMat(confMapZEDMat);
                    pubConfMap.publish(imageToROSmsg(
                                           confMapFloat, sensor_msgs::image_encodings::TYPE_32FC1,
                                           confidenceOptFrameId, t));
                }

                // Publish the point cloud if someone has subscribed to
                if (cloudSubnumber > 0) {
                    // Run the point cloud conversion asynchronously to avoid slowing down
                    // all the program
                    // Retrieve raw pointCloud data
                    zed.retrieveMeasure(mCloud, sl::MEASURE_XYZBGRA, sl::MEM_CPU, matWidth,
                                        matHeight);
                    mPointCloudFrameId = depthFrameId;
                    mPointCloudTime = t;
                    publishPointCloud(matWidth, matHeight);
                }

                mDataMutex.unlock();
                // Transform from base to sensor
                tf2::Transform sensor_to_base_transf;

                // Look up the transformation from base frame to camera link
                try {
                    // Save the transformation from base to frame
                    geometry_msgs::TransformStamped s2b =
                        tfBuffer->lookupTransform(baseFrameId, depthFrameId, t);
                    // Get the TF2 transformation
                    tf2::fromMsg(s2b.transform, sensor_to_base_transf);
                } catch (tf2::TransformException& ex) {
                    NODELET_WARN_THROTTLE(
                        10.0, "The tf from '%s' to '%s' does not seem to be available, "
                        "will assume it as identity!",
                        depthFrameId.c_str(), baseFrameId.c_str());
                    NODELET_DEBUG("Transform error: %s", ex.what());
                    sensor_to_base_transf.setIdentity();
                }

                // Publish the odometry if someone has subscribed to
                if (mTerrainMap || poseSubnumber > 0 || odomSubnumber > 0 || cloudSubnumber > 0 ||
                    depthSubnumber > 0 || imuSubnumber > 0 || imuRawsubnumber > 0) {
                    if (!initOdomWithPose) {
                        sl::Pose deltaOdom;
                        sl::TRACKING_STATE status = zed.getPosition(deltaOdom, sl::REFERENCE_FRAME_CAMERA);

                        if (status == sl::TRACKING_STATE_OK || status == sl::TRACKING_STATE_SEARCHING || status == sl::TRACKING_STATE_FPS_TOO_LOW) {
                            // Transform ZED delta odom pose in TF2 Transformation
                            geometry_msgs::Transform deltaTransf;
                            sl::Translation translation = deltaOdom.getTranslation();
                            sl::Orientation quat = deltaOdom.getOrientation();
                            deltaTransf.translation.x = mSignX * translation(mIdxX);
                            deltaTransf.translation.y = mSignY * translation(mIdxY);
                            deltaTransf.translation.z = mSignZ * translation(mIdxZ);
                            deltaTransf.rotation.x = mSignX * quat(mIdxX);
                            deltaTransf.rotation.y = mSignY * quat(mIdxY);
                            deltaTransf.rotation.z = mSignZ * quat(mIdxZ);
                            deltaTransf.rotation.w = quat(3);
                            tf2::Transform deltaOdomTf;
                            tf2::fromMsg(deltaTransf, deltaOdomTf);
                            // delta odom from sensor to base frame
                            tf2::Transform deltaOdomTf_base = sensor_to_base_transf *
                                                              deltaOdomTf *
                                                              sensor_to_base_transf.inverse();
                            // Propagate Odom transform in time
                            baseToOdomTransform = baseToOdomTransform * deltaOdomTf_base;
                            // Publish odometry message
                            publishOdom(baseToOdomTransform, t);
                            mTrackingReady = true;
                        } else {
                            NODELET_DEBUG_STREAM("ODOM -> Tracking Status: " << static_cast<int>(status));
                        }
                    }
                }

                // Publish the zed camera pose if someone has subscribed to
                if (mTerrainMap || poseSubnumber > 0 || odomSubnumber > 0 || cloudSubnumber > 0 ||
                    depthSubnumber > 0 || imuSubnumber > 0 || imuRawsubnumber > 0) {
                    sl::Pose zed_pose; // Sensor to Map transform
                    sl::TRACKING_STATE status = zed.getPosition(zed_pose, sl::REFERENCE_FRAME_WORLD);

                    if (status == sl::TRACKING_STATE_OK) { // TODO Verify if we should add also TRACKING_STATE_SEARCHING
                        // Transform ZED pose in TF2 Transformation
                        geometry_msgs::Transform sens2mapTransf;
                        sl::Translation translation = zed_pose.getTranslation();
                        sl::Orientation quat = zed_pose.getOrientation();
                        sens2mapTransf.translation.x = mSignX * translation(mIdxX);
                        sens2mapTransf.translation.y = mSignY * translation(mIdxY);
                        sens2mapTransf.translation.z = mSignZ * translation(mIdxZ);
                        sens2mapTransf.rotation.x = mSignX * quat(mIdxX);
                        sens2mapTransf.rotation.y = mSignY * quat(mIdxY);
                        sens2mapTransf.rotation.z = mSignZ * quat(mIdxZ);
                        sens2mapTransf.rotation.w = quat(3);
                        tf2::Transform sens_to_map_transf;
                        tf2::fromMsg(sens2mapTransf, sens_to_map_transf);
                        // Transformation from camera sensor to base frame
                        tf2::Transform base_to_map_transform = sensor_to_base_transf *
                                                               sens_to_map_transf *
                                                               sensor_to_base_transf.inverse();

                        if (initOdomWithPose) {
                            // Propagate Odom transform in time
                            baseToOdomTransform = base_to_map_transform;
                            base_to_map_transform.setIdentity();

                            if (odomSubnumber > 0) {
                                // Publish odometry message
                                publishOdom(baseToOdomTransform, t);
                            }

                            initOdomWithPose = false;
                        } else {
                            // Transformation from map to odometry frame
                            odomToMapTransform =
                                base_to_map_transform * baseToOdomTransform.inverse();
                        }

                        // Publish Pose message
                        publishPose(odomToMapTransform, t);
                        mTrackingReady = true;
                    } else {
                        NODELET_DEBUG_STREAM("MAP -> Tracking Status: " << static_cast<int>(status));
                    }
                }

                // Publish pose tf only if enabled
                if (publishTf) {
                    // Note, the frame is published, but its values will only change if
                    // someone has subscribed to odom
                    publishOdomFrame(baseToOdomTransform,
                                     t); // publish the base Frame in odometry frame
                    // Note, the frame is published, but its values will only change if
                    // someone has subscribed to map
                    publishPoseFrame(odomToMapTransform,
                                     t); // publish the odometry Frame in map frame
                    imuTime = t;
                }

                static int rateWarnCount = 0;

                if (!loop_rate.sleep()) {
                    rateWarnCount++;

                    if (rateWarnCount == 10) {
                        NODELET_DEBUG_THROTTLE(
                            1.0,
                            "Working thread is not synchronized with the Camera frame rate");
                        NODELET_DEBUG_STREAM_THROTTLE(
                            1.0, "Expected cycle time: " << loop_rate.expectedCycleTime()
                            << " - Real cycle time: "
                            << loop_rate.cycleTime());
                        NODELET_WARN_THROTTLE(10.0, "Elaboration takes longer than requested "
                                              "by the FPS rate. Please consider to "
                                              "lower the 'frame_rate' setting.");
                    }
                } else {
                    rateWarnCount = 0;
                }
            } else {
                NODELET_DEBUG_THROTTLE(5.0, "No topics subscribed by users");

                // Publish odometry tf only if enabled
                if (publishTf) {
                    ros::Time t =
                        sl_tools::slTime2Ros(zed.getTimestamp(sl::TIME_REFERENCE_CURRENT));
                    publishOdomFrame(baseToOdomTransform,
                                     t); // publish the base Frame in odometry frame
                    publishPoseFrame(odomToMapTransform,
                                     t); // publish the odometry Frame in map frame
                }

                std::this_thread::sleep_for(
                    std::chrono::milliseconds(10)); // No subscribers, we just wait
                loop_rate.reset();
            }
        } // while loop

        zed.close();
    }

} // namespace
