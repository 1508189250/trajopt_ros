#include <gtest/gtest.h>
#include <trajopt_utils/stl_to_string.hpp>
#include <trajopt/common.hpp>
#include <trajopt/problem_description.hpp>
#include <trajopt_sco/optimizers.hpp>
#include <ctime>
#include <trajopt_utils/eigen_conversions.hpp>
#include <trajopt_utils/clock.hpp>
#include <boost/foreach.hpp>
#include <boost/assign.hpp>
#include <trajopt_utils/config.hpp>
#include <trajopt_test_utils.hpp>
#include <trajopt_scene/kdl_chain_kin.h>
#include <trajopt_scene/bullet_env.h>
#include <trajopt/plot_callback.hpp>
#include <trajopt_utils/logging.hpp>

#include <ros/ros.h>
#include <urdf_parser/urdf_parser.h>
#include <srdfdom/model.h>

using namespace trajopt;
using namespace std;
using namespace util;
using namespace boost::assign;


const std::string ROBOT_DESCRIPTION_PARAM = "robot_description"; /**< Default ROS parameter for robot description */
const std::string ROBOT_SEMANTIC_PARAM = "robot_description_semantic"; /**< Default ROS parameter for robot description */
bool plotting = false; /**< Enable plotting */

class PlanningTest : public testing::TestWithParam<const char*> {
public:
  ros::NodeHandle nh_;
  urdf::ModelInterfaceSharedPtr model_;  /**< URDF Model */
  srdf::ModelSharedPtr srdf_model_;      /**< SRDF Model */
  trajopt_scene::BulletEnvPtr env_;   /**< Trajopt Basic Environment */

  virtual void SetUp()
  {
    std::string urdf_xml_string, srdf_xml_string;
    nh_.getParam(ROBOT_DESCRIPTION_PARAM, urdf_xml_string);
    nh_.getParam(ROBOT_SEMANTIC_PARAM, srdf_xml_string);
    model_ = urdf::parseURDF(urdf_xml_string);

    srdf_model_ = srdf::ModelSharedPtr(new srdf::Model);
    srdf_model_->initString(*model_, srdf_xml_string);
    env_ = trajopt_scene::BulletEnvPtr(new trajopt_scene::BulletEnv);
    assert(model_ != nullptr);
    assert(env_ != nullptr);

    bool success = env_->init(model_, srdf_model_);
    assert(success);

    std::map<const std::string, double> ipos;
    ipos["torso_lift_joint"] = 0.0;
    env_->setState(ipos);

    gLogLevel = util::LevelError;
  }
};


TEST_F(PlanningTest, numerical_ik1)
{
  ROS_DEBUG("PlanningTest, numerical_ik1");
  Json::Value root = readJsonFile(string(DATA_DIR) + "/numerical_ik1.json");

  TrajOptProbPtr prob = ConstructProblem(root, env_);
  ASSERT_TRUE(!!prob);

  BasicTrustRegionSQP opt(prob);
  if (plotting)
  {
    opt.addCallback(PlotCallback(*prob));
  }

  ROS_DEBUG_STREAM("DOF: " << prob->GetNumDOF());
  opt.initialize(DblVec(prob->GetNumDOF(), 0));
  double tStart = GetClock();
  ROS_DEBUG_STREAM("Size: " << opt.x().size());
  ROS_DEBUG_STREAM("Initial Vars: " << toVectorXd(opt.x()).transpose());
  Eigen::Affine3d initial_pose, final_pose, change_base;
  change_base = prob->GetEnv()->getLinkTransform(prob->GetKin()->getBaseLinkName());
  prob->GetKin()->calcFwdKin(initial_pose, change_base, toVectorXd(opt.x()));

  ROS_DEBUG_STREAM("Initial Position: " << initial_pose.translation().transpose());
  OptStatus status = opt.optimize();
  ROS_DEBUG_STREAM("Status: " << sco::statusToString(status));
  prob->GetKin()->calcFwdKin(final_pose, change_base, toVectorXd(opt.x()));

  Eigen::Affine3d goal;
  goal.translation() << 0.4, 0, 0.8;
  goal.linear() = Eigen::Quaterniond(0, 0, 1, 0).toRotationMatrix();

  assert(goal.isApprox(final_pose, 1e-8));

  ROS_DEBUG_STREAM("Final Position: " << final_pose.translation().transpose());
  ROS_DEBUG_STREAM("Final Vars: " << toVectorXd(opt.x()).transpose());
  ROS_DEBUG("planning time: %.3f", GetClock()-tStart);
}

TEST_F(PlanningTest, arm_around_table)
{
  ROS_DEBUG("PlanningTest, arm_around_table");

  Json::Value root = readJsonFile(string(DATA_DIR) + "/arm_around_table.json");

  std::map<const std::string, double> ipos;
  ipos["torso_lift_joint"] = 0;
  ipos["r_shoulder_pan_joint"] = -1.832;
  ipos["r_shoulder_lift_joint"] = -0.332;
  ipos["r_upper_arm_roll_joint"] = -1.011;
  ipos["r_elbow_flex_joint"] = -1.437;
  ipos["r_forearm_roll_joint"] = -1.1;
  ipos["r_wrist_flex_joint"] = -1.926;
  ipos["r_wrist_roll_joint"] = 3.074;
  env_->setState(ipos);

  TrajOptProbPtr prob = ConstructProblem(root, env_);
  ASSERT_TRUE(!!prob);

  trajopt_scene::DistanceResultVector collisions;
  const std::vector<std::string>& joint_names = prob->GetKin()->getJointNames();
  const std::vector<std::string>& link_names = prob->GetKin()->getLinkNames();

  env_->continuousCollisionCheckTrajectory(joint_names, link_names, prob->GetInitTraj(), collisions);
  ROS_DEBUG("Initial trajector number of continuous collisions: %lui\n", collisions.size());
  ASSERT_NE(collisions.size(), 0);

  BasicTrustRegionSQP opt(prob);
  ROS_DEBUG_STREAM("DOF: " << prob->GetNumDOF());
  if (plotting)
  {
    opt.addCallback(PlotCallback(*prob));
  }

  opt.initialize(trajToDblVec(prob->GetInitTraj()));
  double tStart = GetClock();
  opt.optimize();
  ROS_DEBUG("planning time: %.3f", GetClock()-tStart);

  if (plotting)
  {
    prob->GetEnv()->plotClear();
  }

  collisions.clear();
  env_->continuousCollisionCheckTrajectory(joint_names, link_names, getTraj(opt.x(), prob->GetVars()), collisions);
  ROS_DEBUG("Final trajectory number of continuous collisions: %lui\n", collisions.size());
  ASSERT_EQ(collisions.size(), 0);
}


int main(int argc, char** argv)
{
  testing::InitGoogleTest(&argc, argv);
  ros::init(argc, argv, "trajopt_planning_unit");
  ros::NodeHandle pnh("~");

  pnh.param("plotting", plotting, false);
  return RUN_ALL_TESTS();
}
