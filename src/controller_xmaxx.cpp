/* TODO:
 * Update Mobility_msg::PositionTarget
 * Update rosparam read style
 * Update Add checks for NAN
 */

#include <controller_xmaxx/DebugData.h>
#include <controller_xmaxx/ParamsData.h>
// #include <mobility_msgs/Clearance.h>
#include <mavros_msgs/State.h>
#include <nav_msgs/Odometry.h>
#include <ros/ros.h>
#include <cmath>
#include <tuple>
#include "controller_xmaxx/ChangeMode.h"
#include "controllers.hpp"
#include "mobility_msgs/PositionTargetMode.h"
#include "mobility_msgs/ResiliencyLogicStatus.h"
#include "ros/ros.h"
#include "sensor_msgs/Range.h"
#include "std_msgs/Duration.h"

mobility_msgs::PositionTargetMode x_d_;
mavros_msgs::RCOut rc_;
mavros_msgs::State mavros_state_;
nav_msgs::Odometry odom_est_;
geometry_msgs::TwistStamped vel_est_encoders_;
std::vector<double> vel_prev = {0, 0, 0};
geometry_msgs::WrenchStamped accel_est_;
geometry_msgs::TwistStamped ang_vel_lp_;
ros::Time odom_time_stamp_(0);
ros::Time setpoint_time_stamp_(0);
ros::Time last_bottom_clearance_cb_time_(0);
ros::Time last_top_clearance_cb_time_(0);
double fc_highpass_accel = 100;
double fc_lowpass_ang_vel = 10;
double controller_freq;
double callback_timeout_duration;
double setpoint_callback_timeout_duration;
double mass_;
double max_thrust_rating_;
int mobility_mode_ = mobility_msgs::PositionTargetMode::MODE_GROUND;
double bottom_clearance_ = -1;
double top_clearance_ = -1;
ros::Duration total_flight_time_ = ros::Duration(0);
bool imu_only_ = false;

void setpoint_callback(const mobility_msgs::PositionTargetMode::ConstPtr &msg) {
  setpoint_time_stamp_ = msg->header.stamp;
  /* Check if frame id is accurate */
  if (msg->header.frame_id.find("odom")) {
    x_d_ = *msg;
  } else
    ROS_ERROR("Position Target cordinate frame should be odom frame");
}

void rc_callback(const mavros_msgs::RCOut::ConstPtr &msg) { rc_ = *msg; }

void mavros_state_callback(const mavros_msgs::State::ConstPtr &msg) {
  mavros_state_ = *msg;
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

void botClearanceCB(const sensor_msgs::Range &msg) {
  last_bottom_clearance_cb_time_ = ros::Time::now();
  bottom_clearance_ = msg.range;
}

void topClearanceCB(const sensor_msgs::Range &msg) {
  last_top_clearance_cb_time_ = ros::Time::now();
  top_clearance_ = msg.range;
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

  pnh.param<double>("mass", mass_, mass_);
  pnh.param<double>("max_thrust_rating", max_thrust_rating_,
                    max_thrust_rating_);
  ros::Rate loop_rate(controller_freq);

  /* Create subscribers and publishers */
  ros::Publisher attitude_target_pub =
      nh.advertise<mavros_msgs::AttitudeTarget>("mavros/setpoint_raw/attitude",
                                                1);
  ros::Publisher actuator_control_pub =
      nh.advertise<mavros_msgs::ActuatorControl>("mavros/actuator_control", 1);
  ros::Publisher debug_pub =
      pnh.advertise<controller_xmaxx::DebugData>("debug", 1);
  ros::Publisher params_pub =
      pnh.advertise<controller_xmaxx::ParamsData>("params", 1);
  ros::Publisher flight_time_pub =
      pnh.advertise<std_msgs::Duration>("total_flight_time", 1);

  ros::Subscriber x_d_sub =
      nh.subscribe("command/setpoint_raw/local", 1, setpoint_callback);
  ros::Subscriber rc_sub = nh.subscribe("mavros/rc/out", 1, rc_callback);
  ros::Subscriber mavros_state_sub =
      nh.subscribe("mavros/state", 1, mavros_state_callback);
  ros::Subscriber odom_est_sub =
      nh.subscribe("resiliency/odometry", 1, odom_est_callback);
  ros::Subscriber vel_est_encoders_sub =
      nh.subscribe("encoder/velocity_filtered", 1, vel_est_encoders_callback);

  ros::Subscriber bot_clearance_sub =
      nh.subscribe("bottom_clearance", 1, botClearanceCB);
  ros::Subscriber top_clearance_sub =
      nh.subscribe("top_clearance", 1, topClearanceCB);

  ros::Subscriber resiliency_status_sub =
      nh.subscribe("resiliency_logic/status", 1, resiliency_status_callback);

  /* Define attitude_desired msgs and initialize controllers */
  mavros_msgs::AttitudeTarget att_msg;
  controller_xmaxx::DebugData debug_msg;
  controller_xmaxx::ParamsData params_msg;
  RollingControllerDirect rolling_controller;
  FlyingControllerBasic flying_controller;
  ControlInputMessage control_input_message;

  int loop_rate_downsample = 0;

  while (ros::ok()) {
    ros::spinOnce();
    mobility_mode_ = x_d_.mode;
    bool in_offboard = false;
    if (mavros_state_.mode == "OFFBOARD" && mavros_state_.armed)
      in_offboard = true;
    rolling_controller.setOffboard(in_offboard);
    flying_controller.setOffboard(in_offboard);

    if ((ros::Time::now() - setpoint_time_stamp_).toSec() >
        setpoint_callback_timeout_duration) {
      x_d_.velocity.x = 0;
      x_d_.velocity.y = 0;
      x_d_.velocity.z = 0;
      x_d_.acceleration_or_force.x = 0;
      x_d_.acceleration_or_force.y = 0;
      // x_d_.acceleration_or_force.z = 0;

      if (imu_only_ &&
          mobility_mode_ == mobility_msgs::PositionTargetMode::MODE_AIR) {
        x_d_.type_mask = x_d_.type_mask | Goal::IGNORE_PX | Goal::IGNORE_PY;
      }

      ROS_WARN("Controller setpoint callback timeout!!");
    }

    double clearance = sqrt(-1);  // use nan to indicate goal in odom frame
    if (x_d_.frame_id_z.find("ground") != std::string::npos) {
      clearance = bottom_clearance_;
    } else if (x_d_.frame_id_z.find("ceiling") != std::string::npos) {
      clearance = -top_clearance_;
    }

    if (mobility_mode_ == mobility_msgs::PositionTargetMode::MODE_GROUND ||
        mobility_mode_ ==
            mobility_msgs::PositionTargetMode::MODE_ROLLING_BOUNCING) {
      rolling_controller.get_control_input(
          x_d_, rc_, odom_est_, vel_est_encoders_, accel_est_, clearance,
          control_input_message, debug_msg, params_msg);
    } else if (mobility_mode_ == mobility_msgs::PositionTargetMode::MODE_AIR ||
               mobility_mode_ ==
                   mobility_msgs::PositionTargetMode::MODE_FLYING_BOUNCING) {
      flying_controller.get_control_input(
          x_d_, rc_, odom_est_, vel_est_encoders_, accel_est_, clearance,
          control_input_message, debug_msg, params_msg);
    } else {
      ROS_ERROR("Invalid mobility mode received in PositionTargetMode!");
      break;
    }

    /* check callback timeouts.  If no state estimate updates, publish zeros */
    bool callback_timeout = false;
    if ((ros::Time::now() - odom_time_stamp_).toSec() >
        callback_timeout_duration) {
      ROS_ERROR("Odometry estimate callback timeout!");
      callback_timeout = true;
    }

    if (x_d_.frame_id_z.find("ground") != std::string::npos &&
        (ros::Time::now() - last_bottom_clearance_cb_time_).toSec() >
            callback_timeout_duration) {
      ROS_ERROR("Bottom clearance estimate callback timeout!");
      callback_timeout = true;
    }

    /* if callback timeout, publish zeros or safely land */
    if (callback_timeout) {
      if (mobility_mode_ == mobility_msgs::PositionTargetMode::MODE_GROUND ||
          mobility_mode_ ==
              mobility_msgs::PositionTargetMode::MODE_ROLLING_BOUNCING) {
        control_input_message.control_input_type =
            ControlInputType::ACTUATOR_CONTROL;
        control_input_message.actuator_control.group_mix =
            mavros_msgs::ActuatorControl::PX4_MIX_FLIGHT_CONTROL;
        control_input_message.actuator_control.controls[0] = 0;
        control_input_message.actuator_control.controls[1] = 0;
        control_input_message.actuator_control.controls[2] = 0;
        control_input_message.actuator_control.controls[3] = 0;
      } else if (mobility_mode_ ==
                     mobility_msgs::PositionTargetMode::MODE_AIR ||
                 mobility_mode_ ==
                     mobility_msgs::PositionTargetMode::MODE_FLYING_BOUNCING) {
        control_input_message.control_input_type =
            ControlInputType::ATTITUDE_TARGET;
        control_input_message.attitude_target.orientation.x =
            0;  // TODO:  update these to be zero roll/pitch
        control_input_message.attitude_target.orientation.y = 0;
        control_input_message.attitude_target.orientation.z = 0;
        control_input_message.attitude_target.orientation.w = 1;
        control_input_message.attitude_target.thrust =
            (mass_ * 9.81 / max_thrust_rating_) * 0.9;
      }
    }

    /* parse out control_input_message and publish to correct topic */
    if (control_input_message.control_input_type ==
        ControlInputType::ATTITUDE_TARGET) {
      control_input_message.attitude_target.header.stamp = ros::Time::now();
      attitude_target_pub.publish(control_input_message.attitude_target);
    } else if (control_input_message.control_input_type ==
               ControlInputType::ACTUATOR_CONTROL) {
      control_input_message.actuator_control.header.stamp = ros::Time::now();
      actuator_control_pub.publish(control_input_message.actuator_control);
    } else {
      ROS_ERROR("No message in control_input_message.");
    }

    /* keep track of flight time */
    double flying_thrust = mass_ * 9.81 / max_thrust_rating_ * 0.8;
    if (in_offboard &&
        control_input_message.control_input_type ==
            ControlInputType::ATTITUDE_TARGET &&
        !callback_timeout && mavros_state_.armed &&
        control_input_message.attitude_target.thrust > flying_thrust) {
      total_flight_time_ += ros::Duration(1.0 / controller_freq);
    }

    std_msgs::Duration dur_msg;
    dur_msg.data = total_flight_time_;
    flight_time_pub.publish(dur_msg);
    debug_msg.total_flight_time = total_flight_time_.toSec();

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
