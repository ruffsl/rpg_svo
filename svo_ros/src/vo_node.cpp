// This file is part of SVO - Semi-direct Visual Odometry.
//
// Copyright (C) 2014 Christian Forster <forster at ifi dot uzh dot ch>
// (Robotics and Perception Group, University of Zurich, Switzerland).
//
// SVO is free software: you can redistribute it and/or modify it under the
// terms of the GNU General Public License as published by the Free Software
// Foundation, either version 3 of the License, or any later version.
//
// SVO is distributed in the hope that it will be useful, but WITHOUT ANY
// WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
// FOR A PARTICULAR PURPOSE. See the GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.

#include <ros/package.h>
#include <string>
#include <svo/frame_handler_mono.h>
#include <svo/map.h>
#include <svo/config.h>
#include <svo_ros/visualizer.h>
#include <vikit/params_helper.h>
#include <sensor_msgs/Image.h>
#include <sensor_msgs/Imu.h>
#include <std_msgs/String.h>
#include <message_filters/subscriber.h>
#include <message_filters/synchronizer.h>
#include <message_filters/sync_policies/approximate_time.h>
#include <image_transport/image_transport.h>
#include <boost/thread.hpp>
#include <cv_bridge/cv_bridge.h>
#include <Eigen/Core>
#include <vikit/abstract_camera.h>
#include <vikit/camera_loader.h>
#include <vikit/user_input_thread.h>

#include <svo_msgs/GetBetweenPose.h>


#define SSTART           0
#define SGOT_KEY_1       1
#define SGOT_KEY_2       2
#define SGETTING_SCALE   3
#define SDEFAULT         4


namespace svo {

/// SVO Interface
class VoNode
{
public:
  svo::FrameHandlerMono* vo_;
  svo::Visualizer visualizer_;
  bool publish_markers_;                 //!< publish only the minimal amount of info (choice for embedded devices)
  bool publish_dense_input_;
  boost::shared_ptr<vk::UserInputThread> user_input_thread_;
  ros::Subscriber sub_remote_key_;
  std::string remote_input_;
  vk::AbstractCamera* cam_;
  bool quit_;

  ros::Time time_start = ros::Time::now();
  ros::Time time_first, time_second;
  ros::Time time_window = ros::Time(10,0);

  int state = SSTART;

  double svo_scale_;
  double our_scale_;

  VoNode();
  ~VoNode();
  void imgCb(const sensor_msgs::ImageConstPtr& msg);
  bool initCb();
  void processUserActions();

  ros::ServiceClient client;
  svo_msgs::GetBetweenPose srv;

  void remoteKeyCb(const std_msgs::StringConstPtr& key_input);
};

VoNode::VoNode() :
  vo_(NULL),
  publish_markers_(vk::getParam<bool>("svo/publish_markers", true)),
  publish_dense_input_(vk::getParam<bool>("svo/publish_dense_input", false)),
  remote_input_(""),
  cam_(NULL),
  quit_(false)
{
  // Start user input thread in parallel thread that listens to console keys
  if(vk::getParam<bool>("svo/accept_console_user_input", true))
    user_input_thread_ = boost::make_shared<vk::UserInputThread>();

  // Create Camera
  if(!vk::camera_loader::loadFromRosNs("svo", cam_))
    throw std::runtime_error("Camera model not correctly specified.");

  // Get initial position and orientation
  visualizer_.T_world_from_vision_ = Sophus::SE3(
      vk::rpy2dcm(Vector3d(vk::getParam<double>("svo/init_rx", 0.0),
                           vk::getParam<double>("svo/init_ry", 0.0),
                           vk::getParam<double>("svo/init_rz", 0.0))),
      Eigen::Vector3d(vk::getParam<double>("svo/init_tx", 0.0),
                      vk::getParam<double>("svo/init_ty", 0.0),
                      vk::getParam<double>("svo/init_tz", 0.0)));

  // Init VO and start
  vo_ = new svo::FrameHandlerMono(cam_);
  vo_->start();
}

VoNode::~VoNode()
{
  delete vo_;
  delete cam_;
  if(user_input_thread_ != NULL)
    user_input_thread_->stop();
}


bool VoNode::initCb(){
    srv.request.timeA = time_first;
    srv.request.timeB = time_second;

    std::cout<<"\ntime_first:\n"<<(time_first)<<std::endl;
    std::cout<<"\ntime_second:\n"<<(time_second)<<std::endl;

    ROS_INFO("Calling service get_between_pose");

    if (client.call(srv)){
        ROS_INFO("Got inti pose from get_between_pose srv!");
        geometry_msgs::Pose m = srv.response.A2B;;
        Eigen::Affine3d e = Eigen::Translation3d(m.position.x,
                                               m.position.y,
                                               m.position.z) *
              Eigen::Quaterniond(m.orientation.w,
                                 m.orientation.x,
                                 m.orientation.y,
                                 m.orientation.z);

        Eigen::Matrix3d rot = e.rotation();
        Eigen::Vector3d trans = e.translation();

        std::cout<<"Time diff in first and second:\n"<<(time_second-time_first)<<std::endl;
        std::cout<<time_first<<std::endl<<time_second<<std::endl;

        SE3 current_trans = SE3(rot,trans);


        std::cout<<"trans:\n"<<trans<<std::endl;
        std::cout<<"rot:\n"<<rot<<std::endl;

        vo_->InitPose(current_trans);
        our_scale_ = current_trans.matrix().block<3,1>(0,3).norm();
        return true;
    }
    else{
        ROS_ERROR("Failed to call service get_between_pose");
        return false;
    }
}

void VoNode::imgCb(const sensor_msgs::ImageConstPtr& msg)
{
  cv::Mat img;
  try {
    img = cv_bridge::toCvShare(msg, "mono8")->image;
  } catch (cv_bridge::Exception& e) {
    ROS_ERROR("cv_bridge exception: %s", e.what());
  }
  processUserActions();

  double diff1 = msg->header.stamp.toSec() - time_start.toSec();
//  std::cout<<"diff1: "<< diff1 <<std::endl;

  if((diff1 > time_window.toSec()) && state == SSTART){
      vo_->reset();
      vo_->start();
      state = SGOT_KEY_1;
  }
  vo_->addImage(img, msg->header.stamp.toSec());
  svo_scale_ = vo_->svo_scale_;

  if((vo_->stage() == FrameHandlerMono::STAGE_SECOND_FRAME) && state == SGOT_KEY_1)
  {    std::cout<<"the first frame at time: "<<msg->header.stamp.toSec()<<std::endl;
       time_first = msg->header.stamp;
       state = SGOT_KEY_2;
  }

  if((vo_->stage() == FrameHandlerMono::STAGE_DEFAULT_FRAME) && state == SGOT_KEY_2)
  {    //std::cout<<"the second frame at time: "<<msg->header.stamp.toSec()<<std::endl;
       time_second = msg->header.stamp;
       state = SGETTING_SCALE;
  }

  double diff2 = msg->header.stamp.toSec() - time_second.toSec();
//  std::cout<<"diff2: "<< diff2 <<std::endl;
//  std::cout<<"state: "<< state <<std::endl;

  if((diff2 > time_window.toSec()) && state == SGETTING_SCALE){
      std::cout<<"time_first: "<< time_first <<std::endl;
      std::cout<<"time_second: "<< time_second <<std::endl;
      if(VoNode::initCb()){
          state = SDEFAULT;
      }
  }

  visualizer_.publishMinimal(img, vo_->lastFrame(), *vo_, msg->header.stamp.toSec());

  if(publish_markers_){
      visualizer_.visualizeMarkers(vo_->lastFrame(), vo_->coreKeyframes(), vo_->map(), state == SDEFAULT, svo_scale_, our_scale_);
  }

  if(publish_dense_input_)
    visualizer_.exportToDense(vo_->lastFrame());

  if(vo_->stage() == FrameHandlerMono::STAGE_PAUSED)
    usleep(100000);
}

void VoNode::processUserActions()
{
  char input = remote_input_.c_str()[0];
  remote_input_ = "";

  if(user_input_thread_ != NULL)
  {
    char console_input = user_input_thread_->getInput();
    if(console_input != 0)
      input = console_input;
  }

  switch(input)
  {
    case 'q':
      quit_ = true;
      printf("SVO user input: QUIT\n");
      break;
    case 'r':
      vo_->reset();
      time_start = ros::Time::now();
      state = SSTART;
      printf("SVO user input: RESET\n");
      break;
    case 's':
      vo_->start();
      time_start = ros::Time::now();
      state = SSTART;
      printf("SVO user input: START\n");
      break;
    default: ;
  }
}

void VoNode::remoteKeyCb(const std_msgs::StringConstPtr& key_input)
{
  remote_input_ = key_input->data;
}

} // namespace svo

int main(int argc, char **argv)
{
  ros::init(argc, argv, "svo");
  ros::NodeHandle nh;
  std::cout << "create vo_node" << std::endl;
  svo::VoNode vo_node;
  vo_node.client = nh.serviceClient<svo_msgs::GetBetweenPose>("/omnimapper_ros_node/get_between_pose");

  // subscribe to cam msgs
  std::string cam_topic(vk::getParam<std::string>("svo/cam_topic", "camera/image_raw"));
  image_transport::ImageTransport it(nh);
  image_transport::Subscriber it_sub = it.subscribe(cam_topic, 5, &svo::VoNode::imgCb, &vo_node);

  // subscribe to remote input
  vo_node.sub_remote_key_ = nh.subscribe("svo/remote_key", 5, &svo::VoNode::remoteKeyCb, &vo_node);

  // start processing callbacks
  while(ros::ok() && !vo_node.quit_)
  {
    ros::spinOnce();
    // TODO check when last image was processed. when too long ago. publish warning that no msgs are received!
  }

  printf("SVO terminated.\n");
  return 0;
}
