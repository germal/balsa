/* TODO:
 * Update Mobility_msg::PositionTarget
 * Update rosparam read style
 * Update Add checks for NAN
 */

#include <ackermann_msgs/AckermannDriveStamped.h>
#include <controller_xmaxx/DebugData.h>
#include <controller_xmaxx/ParamsData.h>
#include <mobility_msgs/ResiliencyLogicStatus.h>
#include <nav_msgs/Odometry.h>
#include <ros/ros.h>
#include <cmath>
#include <tuple>
#include "controllers.hpp"
#include "ros/ros.h"
#include "std_msgs/Duration.h"

ackermann_msgs::AckermannDriveStamped x_d_;
nav_msgs::Odometry odom_est_;
geometry_msgs::TwistStamped vel_est_encoders_;
std::vector<double> vel_prev = {0, 0, 0};
geometry_msgs::WrenchStamped accel_est_;
geometry_msgs::TwistStamped ang_vel_lp_;
ros::Time odom_time_stamp_(0);
ros::Time setpoint_time_stamp_(0);
double fc_highpass_accel = 100;
double fc_lowpass_ang_vel = 10;
double controller_freq;
double callback_timeout_duration;
double setpoint_callback_timeout_duration;
ros::Duration total_operation_time_ = ros::Duration(0);
int imu_only_;

void setpoint_callback(
    const ackermann_msgs::AckermannDriveStamped::ConstPtr &msg) {
  setpoint_time_stamp_ = msg->header.stamp;
  /* Check if frame id is accurate */
  if (msg->header.frame_id.find("base_link")) {
    x_d_ = *msg;
  } else
    ROS_ERROR("Position Target cordinate frame should be odom frame");
}

void resiliency_status_callback(
    const mobility_msgs::ResiliencyLogicStatus::ConstPtr &msg) {
  imu_only_ = msg->imu_only;
}

void odom_est_callback(const nav_msgs::Odometry::ConstPtr &msg) {
  if (!(msg->pose.pose.position.x != msg->pose.pose.position.x ||
        msg->pose.pose.position.y != msg->pose.pose.position.y ||
        msg->pose.pose.position.z != msg->pose.pose.position.z ||
        msg->pose.pose.orientation.x != msg->pose.pose.orientation.x ||
        msg->pose.pose.orientation.y != msg->pose.pose.orientation.y ||
        msg->pose.pose.orientation.z != msg->pose.pose.orientation.z ||
        msg->pose.pose.orientation.w != msg->pose.pose.orientation.w ||
        msg->twist.twist.linear.x != msg->twist.twist.linear.x ||
        msg->twist.twist.linear.y != msg->twist.twist.linear.y ||
        msg->twist.twist.linear.z != msg->twist.twist.linear.z ||
        msg->twist.twist.angular.x != msg->twist.twist.angular.x ||
        msg->twist.twist.angular.y != msg->twist.twist.angular.y ||
        msg->twist.twist.angular.z != msg->twist.twist.angular.z)) {
    odom_est_ = *msg;

    /* first order filter with moving average to estimate acceleration */
    double dt;
    if (odom_time_stamp_.toSec() == 0)
      dt = 1.0 / controller_freq;
    else
      dt = (odom_est_.header.stamp - odom_time_stamp_).toSec();
    double accel_x = (odom_est_.twist.twist.linear.x - vel_prev[0]) / dt;
    double accel_y = (odom_est_.twist.twist.linear.y - vel_prev[1]) / dt;
    double accel_z = (odom_est_.twist.twist.linear.z - vel_prev[2]) / dt;
    double alpha = dt / (1.0 / (2.0 * 3.14 * fc_highpass_accel) + dt);
    accel_est_.header = odom_est_.header;
    accel_est_.wrench.force.x =
        (1.0 - alpha) * accel_est_.wrench.force.x + alpha * accel_x;
    accel_est_.wrench.force.y =
        (1.0 - alpha) * accel_est_.wrench.force.y + alpha * accel_y;
    accel_est_.wrench.force.z =
        (1.0 - alpha) * accel_est_.wrench.force.z + alpha * accel_z;

    vel_prev[0] = odom_est_.twist.twist.linear.x;
    vel_prev[1] = odom_est_.twist.twist.linear.y;
    vel_prev[2] = odom_est_.twist.twist.linear.z;

    /* first order lowpass filter to smooth out angular velocities */
    double alpha_lp;
    if (odom_time_stamp_.toSec() == 0) {
      dt = 1.0 / controller_freq;
      alpha_lp = dt / (1.0 / (2.0 * 3.14 * fc_lowpass_ang_vel) + dt);
      ang_vel_lp_.twist.angular.x = alpha_lp * odom_est_.twist.twist.angular.x;
      ang_vel_lp_.twist.angular.y = alpha_lp * odom_est_.twist.twist.angular.y;
      ang_vel_lp_.twist.angular.z = alpha_lp * odom_est_.twist.twist.angular.z;
    } else {
      dt = (odom_est_.header.stamp - odom_time_stamp_).toSec();
      alpha_lp = dt / (1.0 / (2.0 * 3.14 * fc_lowpass_ang_vel) + dt);
    }
    ang_vel_lp_.header = odom_est_.header;
    ang_vel_lp_.twist.angular.x = ang_vel_lp_.twist.angular.x +
                                  alpha_lp * (odom_est_.twist.twist.angular.x -
                                              ang_vel_lp_.twist.angular.x);
    ang_vel_lp_.twist.angular.y = ang_vel_lp_.twist.angular.y +
                                  alpha_lp * (odom_est_.twist.twist.angular.y -
                                              ang_vel_lp_.twist.angular.y);
    ang_vel_lp_.twist.angular.z = ang_vel_lp_.twist.angular.z +
                                  alpha_lp * (odom_est_.twist.twist.angular.z -
                                              ang_vel_lp_.twist.angular.z);

    odom_est_.twist.twist.angular.x = ang_vel_lp_.twist.angular.x;
    odom_est_.twist.twist.angular.y = ang_vel_lp_.twist.angular.y;
    odom_est_.twist.twist.angular.z = ang_vel_lp_.twist.angular.z;

    /* update callback timer */
    odom_time_stamp_ = ros::Time::now();
  } else {
    ROS_ERROR("Got NaNs in /resiliency/odometry !");
  }
}

void vel_est_encoders_callback(
    const geometry_msgs::TwistStamped::ConstPtr &msg) {
  if (!(msg->twist.linear.x != msg->twist.linear.x ||
        msg->twist.linear.y != msg->twist.linear.y ||
        msg->twist.linear.z != msg->twist.linear.z ||
        msg->twist.angular.x != msg->twist.angular.x ||
        msg->twist.angular.y != msg->twist.angular.y ||
        msg->twist.angular.z != msg->twist.angular.z)) {
    vel_est_encoders_ = *msg;
  }
}

int main(int argc, char **argv) {
  /* Initialize Ros Node */
  odom_est_.pose.pose.position.x = 0;
  odom_est_.pose.pose.position.y = 0;
  odom_est_.pose.pose.position.z = 0;
  odom_est_.pose.pose.orientation.x = 0;
  odom_est_.pose.pose.orientation.y = 0;
  odom_est_.pose.pose.orientation.z = 0;
  odom_est_.pose.pose.orientation.w = 1.0;
  odom_est_.twist.twist.linear.x = 0;
  odom_est_.twist.twist.linear.y = 0;
  odom_est_.twist.twist.linear.z = 0;
  odom_est_.twist.twist.angular.x = 0;
  odom_est_.twist.twist.angular.y = 0;
  odom_est_.twist.twist.angular.z = 0;
  vel_est_encoders_.twist.linear.x = 0;
  vel_est_encoders_.twist.linear.y = 0;
  vel_est_encoders_.twist.linear.z = 0;
  vel_est_encoders_.twist.angular.x = 0;
  vel_est_encoders_.twist.angular.y = 0;
  vel_est_encoders_.twist.angular.z = 0;
  accel_est_.wrench.force.x = 0;
  accel_est_.wrench.force.y = 0;
  accel_est_.wrench.force.z = 0;
  accel_est_.wrench.torque.x = 0;
  accel_est_.wrench.torque.y = 0;
  accel_est_.wrench.torque.z = 0;

  ros::init(argc, argv, "controller_xmaxx");
  ros::NodeHandle nh, pnh("~");
  pnh.param<double>("controller_freq", controller_freq, controller_freq);
  pnh.param<double>("fc_highpass_accel", fc_highpass_accel, fc_highpass_accel);
  pnh.param<double>("fc_lowpass_ang_vel", fc_lowpass_ang_vel,
                    fc_lowpass_ang_vel);
  pnh.param<double>("callback_timeout_duration", callback_timeout_duration,
                    callback_timeout_duration);
  pnh.param<double>("setpoint_callback_timeout_duration",
                    setpoint_callback_timeout_duration,
                    setpoint_callback_timeout_duration);
  std::cout << callback_timeout_duration << "\n";
  ros::Rate loop_rate(controller_freq);

  /* Create subscribers and publishers */
  ros::Publisher output_pub =
      nh.advertise<ackermann_msgs::AckermannDriveStamped>("output", 1);
  ros::Publisher debug_pub =
      pnh.advertise<controller_xmaxx::DebugData>("debug", 1);
  ros::Publisher params_pub =
      pnh.advertise<controller_xmaxx::ParamsData>("params", 1);
  ros::Publisher operation_time_pub =
      pnh.advertise<std_msgs::Duration>("total_operation_time", 1);

  ros::Subscriber x_d_sub = nh.subscribe("target", 1, setpoint_callback);
  ros::Subscriber odom_est_sub =
      nh.subscribe("resiliency/odometry", 1, odom_est_callback);
  ros::Subscriber vel_est_encoders_sub =
      nh.subscribe("encoder/velocity_filtered", 1, vel_est_encoders_callback);
  ros::Subscriber resiliency_status_sub =
      nh.subscribe("resiliency_logic/status", 1, resiliency_status_callback);

  /* Define attitude_desired msgs and initialize controllers */
  controller_xmaxx::DebugData debug_msg;
  controller_xmaxx::ParamsData params_msg;
  AckermannController controller;
  ackermann_msgs::AckermannDriveStamped output;

  int loop_rate_downsample = 0;

  while (ros::ok()) {
    ros::spinOnce();

    if ((ros::Time::now() - setpoint_time_stamp_).toSec() >
        setpoint_callback_timeout_duration) {
      x_d_.drive.steering_angle = 0;
      x_d_.drive.steering_angle_velocity = 0;
      x_d_.drive.speed = 0;
      x_d_.drive.acceleration = 0;
      x_d_.drive.jerk = 0;

      ROS_WARN("Controller setpoint callback timeout!!");
    }

    controller.get_control_input(x_d_, odom_est_, vel_est_encoders_, accel_est_,
                                 output, debug_msg, params_msg);

    /* check callback timeouts.  If no state estimate updates, publish zeros */
    bool callback_timeout = false;
    if ((ros::Time::now() - odom_time_stamp_).toSec() >
        callback_timeout_duration) {
      ROS_ERROR("Odometry estimate callback timeout!");
      callback_timeout = true;
    }

    /* if callback timeout, publish zeros or safely land */
    if (callback_timeout) {
      output.drive.steering_angle = 0;
      output.drive.steering_angle_velocity = 0;
      output.drive.speed = 0;
      output.drive.acceleration = 0;
      output.drive.jerk = 0;
    }

    /* parse out control_input_message and publish to correct topic */
    output.header.stamp = ros::Time::now();
    output_pub.publish(output);

    /* keep track of operation time */
    if (true) {  // condition for vehicle armed and running
      total_operation_time_ += ros::Duration(1.0 / controller_freq);
    }

    std_msgs::Duration dur_msg;
    dur_msg.data = total_operation_time_;
    operation_time_pub.publish(dur_msg);
    debug_msg.total_operation_time = total_operation_time_.toSec();

    /* publish debug */
    debug_msg.header.stamp = ros::Time::now();
    debug_pub.publish(debug_msg);

    /* publish params at a lower rate, every 10 sec. */
    if (loop_rate_downsample > controller_freq * 10) {
      params_pub.publish(params_msg);
      loop_rate_downsample = 0;
    } else {
      loop_rate_downsample++;
    }

    loop_rate.sleep();
  }
  return 0;
}
