/*********************************************************************
*
* Software License Agreement (BSD License)
*
*  Copyright (c) 2021, Michael Ferguson
*  Copyright (c) 2009, Willow Garage, Inc.
*  All rights reserved.
*
*  Redistribution and use in source and binary forms, with or without
*  modification, are permitted provided that the following conditions
*  are met:
*
*   * Redistributions of source code must retain the above copyright
*     notice, this list of conditions and the following disclaimer.
*   * Redistributions in binary form must reproduce the above
*     copyright notice, this list of conditions and the following
*     disclaimer in the documentation and/or other materials provided
*     with the distribution.
*   * Neither the name of Willow Garage, Inc. nor the names of its
*     contributors may be used to endorse or promote products derived
*     from this software without specific prior written permission.
*
*  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
*  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
*  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
*  FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
*  COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
*  INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
*  BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
*  LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER
*  CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
*  LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
*  ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
*  POSSIBILITY OF SUCH DAMAGE.
*
* Author: Eitan Marder-Eppstein, Michael Ferguson
*********************************************************************/

#include <cmath>
#include <mutex>

#include <nav_core/base_local_planner.h>
#include <nav_msgs/Path.h>

#include <angles/angles.h>
#include <base_local_planner/local_planner_util.h>
#include <base_local_planner/goal_functions.h>
#include <base_local_planner/odometry_helper_ros.h>
#include <graceful_controller/graceful_controller.hpp>
#include <std_msgs/Float32.h>
#include <tf2/utils.h>
#include <tf2_geometry_msgs/tf2_geometry_msgs.h>

#include <dynamic_reconfigure/server.h>
#include <graceful_controller_ros/GracefulControllerConfig.h>

namespace graceful_controller
{

double sign(double x)
{
  return x < 0.0 ? -1.0 : 1.0;
}

class GracefulControllerROS : public nav_core::BaseLocalPlanner
{
public:
  GracefulControllerROS() :
    initialized_(false),
    has_new_path_(false)
  {
  }

  /**
   * @brief Constructs the local planner
   * @param name The name to give this instance of the local planner
   * @param tf A pointer to a transform buffer
   * @param costmap_ros The cost map to use for assigning costs to local plans
   */
  virtual void initialize(std::string name, tf2_ros::Buffer* tf, costmap_2d::Costmap2DROS* costmap_ros)
  {
    if (!initialized_)
    {
      // Publishers (same topics as DWA/TrajRollout)
      ros::NodeHandle private_nh("~/" + name);
      global_plan_pub_ = private_nh.advertise<nav_msgs::Path>("global_plan", 1);
      local_plan_pub_ = private_nh.advertise<nav_msgs::Path>("local_plan", 1);

      buffer_ = tf;
      costmap_ros_ = costmap_ros;
      costmap_2d::Costmap2D* costmap = costmap_ros_->getCostmap();
      planner_util_.initialize(tf, costmap, costmap_ros_->getGlobalFrameID());

      std::string odom_topic;
      if (private_nh.getParam("odom_topic", odom_topic))
      {
        odom_helper_.setOdomTopic(odom_topic);
        private_nh.param("acc_dt", acc_dt_, 0.25);
      }

      bool use_vel_topic = false;
      private_nh.getParam("use_vel_topic", use_vel_topic);
      if (use_vel_topic)
      {
        ros::NodeHandle nh;
        max_vel_sub_ = nh.subscribe<std_msgs::Float32>("max_vel_x", 1,
                         boost::bind(&GracefulControllerROS::velocityCallback, this, _1));
      }

      initialized_ = true;

      // Dynamic reconfigure is really only intended for tuning controller!
      dsrv_ = new dynamic_reconfigure::Server<GracefulControllerConfig>(private_nh);
      dynamic_reconfigure::Server<GracefulControllerConfig>::CallbackType cb =
        boost::bind(&GracefulControllerROS::reconfigureCallback, this, _1, _2);
      dsrv_->setCallback(cb);
    }
    else
    {
      ROS_WARN("This planner has already been initialized, doing nothing.");
    }
  }

  /**
   * @brief Callback for dynamic reconfigure server.
   */
  void reconfigureCallback(GracefulControllerConfig &config, uint32_t level)
  {
    // Lock the mutex
    std::lock_guard<std::mutex> lock(config_mutex_);

    // Update generic local planner params
    base_local_planner::LocalPlannerLimits limits;
    limits.max_vel_trans = config.max_vel_trans;
    limits.min_vel_trans = config.min_vel_trans;
    limits.max_vel_x = config.max_vel_x;
    limits.min_vel_x = config.min_vel_x;
    limits.max_vel_y = config.max_vel_y;
    limits.min_vel_y = config.min_vel_y;
    limits.max_vel_theta = config.max_vel_theta;
    limits.min_vel_theta = config.min_vel_theta;
    limits.acc_lim_x = config.acc_lim_x;
    limits.acc_lim_y = config.acc_lim_y;
    limits.acc_lim_theta = config.acc_lim_theta;
    limits.acc_lim_trans = config.acc_lim_trans;
    limits.xy_goal_tolerance = config.xy_goal_tolerance;
    limits.yaw_goal_tolerance = config.yaw_goal_tolerance;
    limits.prune_plan = config.prune_plan;
    limits.trans_stopped_vel = config.trans_stopped_vel;
    limits.theta_stopped_vel = config.theta_stopped_vel;
    planner_util_.reconfigureCB(limits, false);

    xy_goal_tolerance_ = config.xy_goal_tolerance;
    yaw_goal_tolerance_ = config.yaw_goal_tolerance;
    min_in_place_vel_theta_ = config.min_in_place_vel_theta;
    max_lookahead_ = config.max_lookahead;
    initial_rotate_tolerance_ = config.initial_rotate_tolerance;
    resolution_ = planner_util_.getCostmap()->getResolution();

    // Note: calling dynamic reconfigure will override velocity topic
    max_vel_x_ = config.max_vel_x;

    controller_ = std::make_shared<GracefulController>(config.k1,
                                                       config.k2,
                                                       config.min_vel_x,
                                                       config.max_vel_x,
                                                       config.acc_lim_x,
                                                       config.max_vel_theta,
                                                       config.beta,
                                                       config.lambda);
  }

  /**
   * @brief Given the current position, orientation, and velocity of the robot, compute
   *        velocity commands to send to the base
   * @param cmd_vel Will be filled with the velocity command to be passed to the robot base
   * @return True if a valid velocity command was found, false otherwise
   */
  virtual bool computeVelocityCommands(geometry_msgs::Twist& cmd_vel)
  {
    if (!initialized_)
    {
      ROS_ERROR("Planner is not initialized, call initialize() before using this planner");
      return false;
    }

    // Lock the mutex
    std::lock_guard<std::mutex> lock(config_mutex_);

    if (!costmap_ros_->getRobotPose(robot_pose_))
    {
      ROS_ERROR("Could not get the robot pose");
      return false;
    }

    std::vector<geometry_msgs::PoseStamped> transformed_plan;
    if (!planner_util_.getLocalPlan(robot_pose_, transformed_plan))
    {
      ROS_ERROR("Could not get local plan");
      return false;
    }

    base_local_planner::publishPlan(transformed_plan, global_plan_pub_);

    if (transformed_plan.empty())
    {
      ROS_WARN("Received an empty transform plan");
      return false;
    }

    // Get transforms
    geometry_msgs::TransformStamped odom_to_base, base_to_odom;
    try
    {
      odom_to_base = buffer_->lookupTransform(costmap_ros_->getBaseFrameID(),
                                              costmap_ros_->getGlobalFrameID(),
                                              ros::Time(),
                                              ros::Duration(0.5));
      tf2::Stamped<tf2::Transform> tf2_odom_to_base;
      tf2::convert(odom_to_base, tf2_odom_to_base);
      tf2_odom_to_base.setData(tf2_odom_to_base.inverse());
      base_to_odom = tf2::toMsg(tf2_odom_to_base);
    }
    catch (tf2::TransformException& ex)
    {
      ROS_ERROR("Could not transform to %s", costmap_ros_->getBaseFrameID().c_str());
      return false;
    }

    double dx = transformed_plan.back().pose.position.x - robot_pose_.pose.position.x;
    double dy = transformed_plan.back().pose.position.y - robot_pose_.pose.position.y;
    if (std::hypot(dx, dy) < xy_goal_tolerance_)
    {
      // XY goal tolerance reached - now just rotate towards goal
      geometry_msgs::PoseStamped goal;
      tf2::doTransform(transformed_plan.back(), goal, odom_to_base);
      rotateTowards(goal, cmd_vel);
      return true;
    }

    // Work back from the end of plan
    for (size_t i = transformed_plan.size() - 1; i > 0; --i)
    {
      geometry_msgs::PoseStamped pose = transformed_plan[i];

      // Continue if this is too far away
      dx = pose.pose.position.x - robot_pose_.pose.position.x;
      dy = pose.pose.position.y - robot_pose_.pose.position.y;
      if (std::hypot(dx, dy) > max_lookahead_)
      {
        continue;
      }

      // Transform pose into base_link
      tf2::doTransform(pose, pose, odom_to_base);

      if (has_new_path_ && initial_rotate_tolerance_ > 0.0)
      {
        // Rotate towards goal
        if (fabs(rotateTowards(pose, cmd_vel)) < initial_rotate_tolerance_)
        {
          ROS_INFO("Done rotating towards path");
          has_new_path_ = false;
        }
        else
        {
          return true;
        }
      }

      // Configure controller max velocity based on current speed
      if (!odom_helper_.getOdomTopic().empty())
      {
        base_local_planner::LocalPlannerLimits limits = planner_util_.getCurrentLimits();
        geometry_msgs::PoseStamped robot_velocity;
        odom_helper_.getRobotVel(robot_velocity);
        double max_vel_x = robot_velocity.pose.position.x + (limits.acc_lim_x * acc_dt_);
        max_vel_x = std::min(max_vel_x, max_vel_x_);
        max_vel_x = std::max(max_vel_x, limits.min_vel_x);
        controller_->setVelocityLimits(limits.min_vel_x, max_vel_x, limits.max_vel_theta);
      }

      // Simulated path (for debugging/visualization)
      std::vector<geometry_msgs::PoseStamped> path;
      // Get control and path, iteratively
      while (true)
      {
        // The error between current robot pose and the lookahead goal
        geometry_msgs::PoseStamped error = pose;

        // Extract error_angle
        double error_angle = tf2::getYaw(error.pose.orientation);

        // Move origin to our current simulated pose
        if (!path.empty())
        {
          double x = error.pose.position.x - path.back().pose.position.x;
          double y = error.pose.position.y - path.back().pose.position.y;

          double theta = -tf2::getYaw(path.back().pose.orientation);
          error.pose.position.x = x * cos(theta) - y * sin(theta);
          error.pose.position.y = y * cos(theta) + x * sin(theta);

          error_angle += theta;
          error.pose.orientation.z = sin(error_angle / 2.0);
          error.pose.orientation.w = cos(error_angle / 2.0);
        }

        // Compute commands
        double vel_x, vel_th;
        if (!controller_->approach(error.pose.position.x, error.pose.position.y, error_angle,
                                   vel_x, vel_th))
        {
          ROS_ERROR("Unable to compute approach");
          return false;
        }

        if (path.empty())
        {
          cmd_vel.linear.x = vel_x;
          cmd_vel.angular.z = vel_th;
        }
        else if (std::hypot(error.pose.position.x, error.pose.position.y) < resolution_)
        {
          // We have reached lookahead goal without collision
          base_local_planner::publishPlan(path, local_plan_pub_);
          return true;
        }

        // Forward simulate command
        geometry_msgs::PoseStamped next_pose;
        next_pose.header.frame_id = costmap_ros_->getBaseFrameID();
        if (path.empty())
        {
          // Initialize at origin
          next_pose.pose.orientation.w = 1.0;
        }
        else
        {
          // Start at last pose
          next_pose = path.back();
        }

        // Generate next pose
        double dt = resolution_ / vel_x;
        double yaw = tf2::getYaw(next_pose.pose.orientation);
        next_pose.pose.position.x += dt * vel_x * cos(yaw);
        next_pose.pose.position.y += dt * vel_x * sin(yaw);
        yaw += dt * vel_th;
        next_pose.pose.orientation.z = sin(yaw / 2.0);
        next_pose.pose.orientation.w = cos(yaw / 2.0);
        path.push_back(next_pose);

        // Check next pose for collision
        tf2::doTransform(next_pose, next_pose, base_to_odom);
        unsigned mx, my;
        if (!planner_util_.getCostmap()->worldToMap(next_pose.pose.position.x, next_pose.pose.position.y, mx, my))
        {
          ROS_DEBUG("Path is off costmap (%f,%f)", next_pose.pose.position.x, next_pose.pose.position.y);
          break;
        }
        if (planner_util_.getCostmap()->getCost(mx, my) >= costmap_2d::INSCRIBED_INFLATED_OBSTACLE)
        {
          // Collision - can't go this far!
          ROS_DEBUG("Collision along path at (%f,%f)", next_pose.pose.position.x, next_pose.pose.position.y);
          break;
        }
      }
    }

    ROS_ERROR("No pose in path was reachable");
    return false;
  }

  /**
   * @brief Check if the goal pose has been achieved by the local planner
   * @return True if achieved, false otherwise
   */
  virtual bool isGoalReached()
  {
    if (!initialized_)
    {
      ROS_ERROR("Planner is not initialized, call initialize() before using this planner");
      return false;
    }

    if (!costmap_ros_->getRobotPose(robot_pose_))
    {
      ROS_ERROR("Could not get the robot pose");
      return false;
    }

    geometry_msgs::PoseStamped goal;
    if (!planner_util_.getGoal(goal))
    {
      ROS_ERROR("Unable to get goal");
      return false;
    }

    double dist = std::hypot(goal.pose.position.x - robot_pose_.pose.position.x,
                             goal.pose.position.y - robot_pose_.pose.position.y);

    double angle = angles::shortest_angular_distance(tf2::getYaw(goal.pose.orientation),
                                                     tf2::getYaw(robot_pose_.pose.orientation));

    return (dist < xy_goal_tolerance_) && (fabs(angle) < yaw_goal_tolerance_);
  }

  /**
   * @brief Set the plan that the local planner is following
   * @param plan The plan to pass to the local planner
   * @return True if the plan was updated successfully, false otherwise
   */
  virtual bool setPlan(const std::vector<geometry_msgs::PoseStamped>& plan)
  {
    if (!initialized_)
    {
      ROS_ERROR("Planner is not initialized, call initialize() before using this planner");
      return false;
    }

    // We need orientations on our poses
    std::vector<geometry_msgs::PoseStamped> oriented_plan;
    oriented_plan.resize(plan.size());

    // Copy the only oriented pose
    oriented_plan.back() = plan.back();

    // For each pose, point at the next one
    for (size_t i = 0; i < oriented_plan.size() - 1; ++i)
    {
      oriented_plan[i] = plan[i];
      double dx = plan[i+1].pose.position.x - plan[i].pose.position.x;
      double dy = plan[i+1].pose.position.y - plan[i].pose.position.y;
      double yaw = std::atan2(dy, dx);
      oriented_plan[i].pose.orientation.z = sin(yaw / 2.0);
      oriented_plan[i].pose.orientation.w = cos(yaw / 2.0);
    }

    // TODO: filter noisy orientations?

    if (planner_util_.setPlan(oriented_plan))
    {
      has_new_path_ = true;
      ROS_INFO("Recieved a new path with %lu points", oriented_plan.size());
      return true;
    }

    // Signal error
    return false;
  }

  /**
   * @brief Rotate the robot towards a goal.
   * @param pose The pose should always be in base frame!
   * @param cmd_vel The returned command velocity
   * @returns The computed angular error.
   */
  double rotateTowards(const geometry_msgs::PoseStamped& pose,
                       geometry_msgs::Twist& cmd_vel)
  {
    // Determine error
    double yaw = 0.0;
    if (std::hypot(pose.pose.position.x, pose.pose.position.y) > 0.5)
    {
      // Goal is far away, point towards goal
      yaw = std::atan2(pose.pose.position.y, pose.pose.position.x);
    }
    else
    {
      // Goal is nearby, align heading
      tf2::Quaternion orientation;
      yaw = tf2::getYaw(pose.pose.orientation);
    }

    ROS_DEBUG("Rotating towards goal, error = %f", yaw);

    // Get limits so we can compute velocity
    base_local_planner::LocalPlannerLimits limits = planner_util_.getCurrentLimits();

    // Determine max velocity based on current speed
    double max_vel_th = limits.max_vel_theta;
    if (!odom_helper_.getOdomTopic().empty())
    {
      geometry_msgs::PoseStamped robot_velocity;
      odom_helper_.getRobotVel(robot_velocity);
      double abs_vel = fabs(tf2::getYaw(robot_velocity.pose.orientation));
      double acc_limited = abs_vel + (limits.acc_lim_theta * acc_dt_);
      max_vel_th = std::min(max_vel_th, acc_limited);
      max_vel_th = std::max(max_vel_th, min_in_place_vel_theta_);
    }

    cmd_vel.linear.x = 0.0;
    cmd_vel.angular.z = sqrt(2 * limits.acc_lim_theta * fabs(yaw));
    cmd_vel.angular.z = sign(yaw) * std::min(max_vel_th, std::max(min_in_place_vel_theta_, cmd_vel.angular.z));

    // Return error
    return yaw;
  }

private:
  void velocityCallback(const std_msgs::Float32::ConstPtr& max_vel_x)
  {
    // Lock the mutex
    std::lock_guard<std::mutex> lock(config_mutex_);

    base_local_planner::LocalPlannerLimits limits = planner_util_.getCurrentLimits();
    max_vel_x_ = std::max(std::min(static_cast<double>(max_vel_x->data), limits.max_vel_x), limits.min_vel_x);
  }

  ros::Publisher global_plan_pub_, local_plan_pub_;
  ros::Subscriber max_vel_sub_;

  bool initialized_;
  GracefulControllerPtr controller_;

  tf2_ros::Buffer* buffer_;
  costmap_2d::Costmap2DROS* costmap_ros_;
  base_local_planner::LocalPlannerUtil planner_util_;
  base_local_planner::OdometryHelperRos odom_helper_;

  // Parameters and dynamic reconfigure
  std::mutex config_mutex_;
  dynamic_reconfigure::Server<GracefulControllerConfig> *dsrv_;
  double max_vel_x_;
  double min_in_place_vel_theta_;
  double xy_goal_tolerance_;
  double yaw_goal_tolerance_;
  double max_lookahead_;
  double resolution_;
  double acc_dt_;

  // Controls initial rotation towards path
  double initial_rotate_tolerance_;
  bool has_new_path_;

  geometry_msgs::PoseStamped robot_pose_;
};

}  // namespace graceful_controller

#include <pluginlib/class_list_macros.h>
PLUGINLIB_EXPORT_CLASS(graceful_controller::GracefulControllerROS, nav_core::BaseLocalPlanner)
