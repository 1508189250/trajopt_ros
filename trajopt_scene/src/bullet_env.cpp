#include "trajopt_scene/bullet_env.h"
#include "trajopt_scene/kdl_chain_kin.h"
#include <moveit_msgs/DisplayTrajectory.h>
#include <geometric_shapes/shape_operations.h>
#include <eigen_conversions/eigen_msg.h>
#include <iostream>
#include <limits>
#include <octomap/octomap.h>

namespace trajopt_scene
{

using Eigen::MatrixXd;
using Eigen::VectorXd;

bool BulletEnv::init(const urdf::ModelInterfaceConstSharedPtr urdf_model)
{
  init(urdf_model, nullptr);
}

bool BulletEnv::init(const urdf::ModelInterfaceConstSharedPtr urdf_model, const srdf::ModelConstSharedPtr srdf_model)
{
  ros::NodeHandle nh;
  initialized_ = false;
  model_ = urdf_model;

  if(model_ == nullptr)
  {
    ROS_ERROR_STREAM("Null pointer to URDF Model");
    return initialized_;
  }

  if (!model_->getRoot())
  {
    ROS_ERROR("Invalid URDF in ROSBulletEnv::init call");
    return initialized_;
  }

  KDL::Tree *kdl_tree = new KDL::Tree();
  if (!kdl_parser::treeFromUrdfModel(*model_, *kdl_tree))
  {
    ROS_ERROR("Failed to initialize KDL from URDF model");
    return initialized_;
  }
  kdl_tree_ = boost::shared_ptr<KDL::Tree>(kdl_tree);
  initialized_ = true;

  if (initialized_)
  {
    for (auto& link : urdf_model->links_)
    {
      if (link.second->collision_array.size() > 0)
      {
        COWPtr new_cow(new COW(link.second.get()));
        if (new_cow)
        {
          setContactDistance(new_cow, BULLET_DEFAULT_CONTACT_DISTANCE);
          robot_link2cow_[new_cow->getID()] = new_cow;
          ROS_DEBUG("Added collision object for link %s", link.second->name.c_str());
        }
        else
        {
          ROS_WARN("ignoring link %s", link.second->name.c_str());
        }
      }
    }

    current_state_ = EnvStatePtr(new EnvState());
    kdl_jnt_array_.resize(kdl_tree_->getNrOfJoints());
    int j = 0;
    for (const auto& seg : kdl_tree_->getSegments())
    {
      const KDL::Joint &jnt = seg.second.segment.getJoint();

      if (jnt.getType() == KDL::Joint::None) continue;
      joint_to_qnr_.insert(std::make_pair(jnt.getName(), seg.second.q_nr));
      kdl_jnt_array_(seg.second.q_nr) = 0.0;
      current_state_->joints.insert(std::make_pair(jnt.getName(), 0.0));

      j++;
    }

    calculateTransforms(current_state_->transforms, kdl_jnt_array_, kdl_tree_->getRootSegment(), Eigen::Affine3d::Identity());
  }

  if (srdf_model != nullptr)
  {
    srdf_model_ = srdf_model;
    for (const auto& group: srdf_model_->getGroups())
    {
      for (const auto& chain: group.chains_)
      {
        KDLChainKinPtr manip(new KDLChainKin());
        manip->init(model_, chain.first, chain.second, group.name_);
        manipulators_.insert(std::make_pair(group.name_, manip));
      }
    }

    // TODO: Need to add other options

    // Populate allowed collision matrix
    for (const auto& pair: srdf_model_->getDisabledCollisionPairs())
    {
      addAllowedCollision(pair.link1_, pair.link2_, pair.reason_);
    }
  }

  scene_pub_ = nh.advertise<moveit_msgs::DisplayRobotState>("/trajopt/scene", 1, true);
  trajectory_pub_ = nh.advertise<moveit_msgs::DisplayTrajectory>("/trajopt/display_planned_path", 1, true);
  collisions_pub_ = nh.advertise<visualization_msgs::MarkerArray>("/trajopt/display_collisions", 1, true);
  arrows_pub_ = nh.advertise<visualization_msgs::MarkerArray>("/trajopt/display_arrows", 1, true);
  axes_pub_ = nh.advertise<visualization_msgs::MarkerArray>("/trajopt/display_axes", 1, true);
  return initialized_;
}

void BulletEnv::calcDistancesDiscrete(const DistanceRequest &req, DistanceResultVector &dists) const
{
  BulletManager manager;
  BulletDistanceMap res;
  BulletDistanceData collisions(&req, &res);

  std::vector<std::string> active_objects;

  EnvStateConstPtr state = getState(req.joint_names, req.joint_angles1);
  constructBulletObject(manager.m_link2cow, active_objects, req.contact_distance, state, req.link_names);

  manager.processCollisionObjects();

  for (auto& obj : active_objects)
  {
    COWPtr cow = manager.m_link2cow[obj];
    assert(cow);

    manager.contactDiscreteTest(cow, collisions);

    if (collisions.done) break;
  }

  dists.reserve(req.link_names.size());
  for (BulletDistanceMap::iterator pair = res.begin(); pair != res.end(); ++pair)
  {
    for (auto it = pair->second.begin(); it != pair->second.end(); ++it)
    {
      DistanceResult d;
      d.distance = it->distance;
      d.valid = true;

      // Note: for trajopt ROSBulletEnv is only aware of links in the urdf so if attached link set link name to parent link name
      if (it->body_types[0] == BodyType::ROBOT_ATTACHED)
      {
        d.link_names[0] = getAttachedBody(it->link_names[0])->info.parent_link_name;
      }
      else
      {
        d.link_names[0] = it->link_names[0];
      }

      // Note: for trajopt ROSBulletEnv is only aware of links in the urdf so if attached link set link name to parent link name
      if (it->body_types[1] == BodyType::ROBOT_ATTACHED)
      {
        d.link_names[1] = getAttachedBody(it->link_names[1])->info.parent_link_name;
      }
      else
      {
        d.link_names[1] = it->link_names[1];
      }

      d.nearest_points[0] = it->nearest_points[0];
      d.nearest_points[1] = it->nearest_points[1];
      d.normal = it->normal;

      if ((d.nearest_points[0].array().isNaN()).all() || (d.nearest_points[1].array().isNaN()).all() || (d.normal.array().isNaN()).all())
        d.valid = false;

      dists.push_back(d);
    }
  }
}

void BulletEnv::calcDistancesContinuous(const DistanceRequest &req, DistanceResultVector &dists) const
{
  BulletManager manager;
  BulletDistanceMap res;
  BulletDistanceData collisions(&req, &res);

  std::vector<std::string> active_objects;

  EnvStateConstPtr state1 = getState(req.joint_names, req.joint_angles1);
  EnvStateConstPtr state2 = getState(req.joint_names, req.joint_angles2);

  constructBulletObject(manager.m_link2cow, active_objects, req.contact_distance, state1, state2, req.link_names);
  manager.processCollisionObjects();

  for (auto& obj : active_objects)
  {
    COWPtr cow = manager.m_link2cow[obj];
    assert(cow);

    manager.contactCastTest(cow, collisions);

    if (collisions.done) break;
  }

  dists.reserve(req.link_names.size());
  for (BulletDistanceMap::iterator pair = res.begin(); pair != res.end(); ++pair)
  {
    for (auto it = pair->second.begin(); it != pair->second.end(); ++it)
    {
      DistanceResult d;
      d.distance = it->distance;
      d.valid = true;

      // Note: for trajopt ROSBulletEnv is only aware of links in the urdf so if attached link set link name to parent link name
      if (it->body_types[0] == BodyType::ROBOT_ATTACHED)
      {
        d.link_names[0] = getAttachedBody(it->link_names[0])->info.parent_link_name;
      }
      else
      {
        d.link_names[0] = it->link_names[0];
      }

      // Note: for trajopt ROSBulletEnv is only aware of links in the urdf so if attached link set link name to parent link name
      if (it->body_types[1] == BodyType::ROBOT_ATTACHED)
      {
        d.link_names[1] = getAttachedBody(it->link_names[1])->info.parent_link_name;
      }
      else
      {
        d.link_names[1] = it->link_names[1];
      }

      d.nearest_points[0] = it->nearest_points[0];
      d.nearest_points[1] = it->nearest_points[1];
      d.normal = it->normal;
      d.cc_type = it->cc_type;
      d.cc_nearest_points[0] = it->cc_nearest_points[0];
      d.cc_nearest_points[1] = it->cc_nearest_points[1];
      d.cc_time = it->cc_time;

      if ((d.nearest_points[0].array().isNaN()).all() || (d.nearest_points[1].array().isNaN()).all() || (d.normal.array().isNaN()).all())
        d.valid = false;

      if (d.cc_type != ContinouseCollisionType::CCType_None && ((d.cc_nearest_points[0].array().isNaN()).all() || (d.cc_nearest_points[1].array().isNaN()).all()))
        d.valid = false;

      dists.push_back(d);
    }
  }
}

//std::set<urdf::LinkConstSharedPtr> ROSBulletEnv::getLinkModels(const std::vector<std::string> &link_names) const
//{
//  std::set<urdf::LinkConstSharedPtr> list;

//  for(auto const& link_name: link_names)
//  {
//    list.insert(model_->getLink(link_name));
//  }

//  return list;
//}

void BulletEnv::calcCollisionsDiscrete(const DistanceRequest &req, std::vector<DistanceResult> &collisions) const
{
  calcDistancesDiscrete(req, collisions);
}

void BulletEnv::calcCollisionsContinuous(const DistanceRequest &req, std::vector<DistanceResult> &collisions) const
{
  calcDistancesContinuous(req, collisions);
}

bool BulletEnv::continuousCollisionCheckTrajectory(const std::vector<std::string> &joint_names, const std::vector<std::string> &link_names, const TrajArray &traj, DistanceResult& collision) const
{
  DistanceRequest req;
  req.type = DistanceRequestType::SINGLE;
  req.joint_names = joint_names;
  req.link_names = link_names;
  req.acm = getAllowedCollisions();

  DistanceResultVector collisions;
  for (int iStep = 0; iStep < traj.rows() - 1; ++iStep)
  {
    req.joint_angles1 = traj.row(iStep);
    req.joint_angles2 = traj.row(iStep + 1);
    calcCollisionsContinuous(req, collisions);
    if (collisions.size() > 0)
    {
      collision = collisions.front();
      return true;
    }
  }
  return false;
}

bool BulletEnv::continuousCollisionCheckTrajectory(const std::vector<std::string> &joint_names, const std::vector<std::string> &link_names, const TrajArray& traj, DistanceResultVector& collisions) const
{
  DistanceRequest req;
  req.type = DistanceRequestType::ALL;
  req.joint_names = joint_names;
  req.link_names = link_names;
  req.acm = getAllowedCollisions();

  bool found = false;
  for (int iStep = 0; iStep < traj.rows() - 1; ++iStep)
  {
    req.joint_angles1 = traj.row(iStep);
    req.joint_angles2 = traj.row(iStep + 1);
    calcCollisionsContinuous(req, collisions);
    if (collisions.size() > 0)
    {
      found = true;
    }
  }
  return found;
}

void BulletEnv::setState(const std::map<const std::string, double> &joints)
{
  current_state_->joints.insert(joints.begin(), joints.end());

  for (auto& joint : joints)
  {
    if (setJointValuesHelper(kdl_jnt_array_, joint.first, joint.second))
    {
      current_state_->joints[joint.first] = joint.second;
    }
  }

  calculateTransforms(current_state_->transforms, kdl_jnt_array_, kdl_tree_->getRootSegment(), Eigen::Affine3d::Identity());
}

void BulletEnv::setState(const std::vector<std::string> &joint_names, const Eigen::VectorXd &joint_values)
{

  for (auto i = 0; i < joint_names.size(); ++i)
  {
    if (setJointValuesHelper(kdl_jnt_array_, joint_names[i], joint_values[i]))
    {
      current_state_->joints[joint_names[i]] = joint_values[i];
    }
  }

  calculateTransforms(current_state_->transforms, kdl_jnt_array_, kdl_tree_->getRootSegment(), Eigen::Affine3d::Identity());
}

EnvStatePtr BulletEnv::getState(const std::map<const std::string, double> &joints) const
{
  EnvStatePtr state(new EnvState(*current_state_));
  KDL::JntArray jnt_array = kdl_jnt_array_;

  for (auto& joint : joints)
  {
    if (setJointValuesHelper(jnt_array, joint.first, joint.second))
    {
      state->joints[joint.first] = joint.second;
    }
  }

  calculateTransforms(state->transforms, jnt_array, kdl_tree_->getRootSegment(), Eigen::Affine3d::Identity());

  return state;
}

EnvStatePtr BulletEnv::getState(const std::vector<std::string> &joint_names, const Eigen::VectorXd &joint_values) const
{
  EnvStatePtr state(new EnvState(*current_state_));
  KDL::JntArray jnt_array = kdl_jnt_array_;

  for (auto i = 0; i < joint_names.size(); ++i)
  {
    if (setJointValuesHelper(jnt_array, joint_names[i], joint_values[i]))
    {
      state->joints[joint_names[i]] = joint_values[i];
    }
  }

  calculateTransforms(state->transforms, jnt_array, kdl_tree_->getRootSegment(), Eigen::Affine3d::Identity());

  return state;
}

Eigen::VectorXd BulletEnv::getCurrentJointValues(const std::string &manipulator_name) const
{
  auto it = manipulators_.find(manipulator_name);
  if (it != manipulators_.end())
  {
    const std::vector<std::string>& joint_names = it->second->getJointNames();
    Eigen::VectorXd start_pos(joint_names.size());

    for(auto j = 0u; j < joint_names.size(); ++j)
    {
      start_pos(j) = current_state_->joints[joint_names[j]];
    }

    return start_pos;
  }

  return Eigen::VectorXd();
}

Eigen::VectorXd BulletEnv::getCurrentJointValues() const
{
  const std::map<const std::string, double>& jv = current_state_->joints;
  Eigen::VectorXd start_pos(jv.size());
  int j = 0;

  for (const auto& joint : jv)
  {
    start_pos(j) = joint.second;
    j++;
  }

  return start_pos;
}

Eigen::Affine3d BulletEnv::getLinkTransform(const std::string& link_name) const
{
  return current_state_->transforms[link_name];
}

bool BulletEnv::addManipulator(const std::string &base_link, const std::string &tip_link, const std::string &manipulator_name)
{
  if (!hasManipulator(manipulator_name))
  {
    KDLChainKinPtr manip(new KDLChainKin());
    manip->init(model_, base_link, tip_link, manipulator_name);

    manipulators_.insert(std::make_pair(manipulator_name, manip));
    return true;
  }
  return false;
}

bool BulletEnv::hasManipulator(const std::string &manipulator_name) const
{
  return manipulators_.find(manipulator_name) != manipulators_.end();
}

BasicKinConstPtr BulletEnv::getManipulator(const std::string &manipulator_name) const
{
  auto it = manipulators_.find(manipulator_name);
  if (it != manipulators_.end())
    return it->second;

  return nullptr;
}

std::string BulletEnv::getManipulatorName(const std::vector<std::string> &joint_names) const
{
  std::set<std::string> joint_names_set(joint_names.begin(), joint_names.end());
  for (const auto& manip : manipulators_)
  {
    const std::vector<std::string>& tmp_joint_names = manip.second->getJointNames();
    std::set<std::string> tmp_joint_names_set(tmp_joint_names.begin(), tmp_joint_names.end());
    if (joint_names_set == tmp_joint_names_set)
      return manip.first;

  }
  return "";
}

void BulletEnv::addAttachableObject(const AttachableObjectConstPtr &attachable_object)
{
  const auto object = attachable_objects_.find(attachable_object->name);
  if (object != attachable_objects_.end())
  {
    ROS_ERROR("Tried to add attachable object %s which which already exists!", attachable_object->name.c_str());
    return;
  }

  attachable_objects_.insert(std::make_pair(attachable_object->name, attachable_object));
}

const AttachedBodyConstPtr BulletEnv::getAttachedBody(const std::string& name) const
{
  const auto body = attached_bodies_.find(name);
  if (body == attached_bodies_.end())
    ROS_ERROR("Tried to get attached body %s which does not exist!", name.c_str());

  return body->second;
}

void BulletEnv::attachBody(const AttachedBodyInfo &attached_body_info)
{
  const auto body_info = attached_bodies_.find(attached_body_info.name);
  const auto obj = attachable_objects_.find(attached_body_info.object_name);

  if (body_info != attached_bodies_.end())
  {
    ROS_ERROR("Tried to attached body %s which is already attached!", attached_body_info.name.c_str());
    return;
  }

  if (obj == attachable_objects_.end())
  {
    ROS_ERROR("Tried to attached body %s with object %s which does not exist!", attached_body_info.name.c_str(), attached_body_info.object_name.c_str());
    return;
  }

  AttachedBodyPtr attached_body(new AttachedBody());
  attached_body->info = attached_body_info;
  attached_body->obj = obj->second;

  attached_bodies_.insert(std::make_pair(attached_body_info.name, attached_body));
  auto it = attached_bodies_.find(attached_body_info.name);
  COWPtr new_cow(new COW(it->second.get()));
  if (new_cow)
  {
    setContactDistance(new_cow, BULLET_DEFAULT_CONTACT_DISTANCE);
    attached_link2cow_[new_cow->getID()] = new_cow;
    ROS_DEBUG("Added collision object for attached body %s", attached_body_info.name.c_str());
  }
  else
  {
    ROS_WARN("Error creating attached body %s", attached_body_info.name.c_str());
  }
}

void BulletEnv::detachBody(const std::string &name)
{
  attached_bodies_.erase(name);
  attached_link2cow_.erase(name);
}

bool BulletEnv::setJointValuesHelper(KDL::JntArray &q, const std::string &joint_name, const double &joint_value) const
{
  auto qnr = joint_to_qnr_.find(joint_name);
  if (qnr != joint_to_qnr_.end())
  {
    q(qnr->second) = joint_value;
    return true;
  }
  else
  {
    ROS_ERROR("Tried to set joint name %s which does not exist!", joint_name.c_str());
    return false;
  }
}

void BulletEnv::calculateTransforms(std::map<const std::string, Eigen::Affine3d> &transforms, const KDL::JntArray& q_in, const KDL::SegmentMap::const_iterator& it, const Eigen::Affine3d& parent_frame) const
{
  if (it != kdl_tree_->getSegments().end())
  {
    const KDL::TreeElementType& current_element = it->second;
    KDL::Frame current_frame = GetTreeElementSegment(current_element).pose(q_in(GetTreeElementQNr(current_element)));

    Eigen::Affine3d local_frame, global_frame;
    KDLChainKin::KDLToEigen(current_frame, local_frame);
    global_frame =  parent_frame * local_frame;
    transforms[current_element.segment.getName()] = global_frame;

    for (auto& child: current_element.children)
    {
      calculateTransforms(transforms, q_in, child, global_frame);
    }
  }
}

void BulletEnv::constructBulletObject(Link2Cow &collision_objects, std::vector<std::string> &active_objects, double contact_distance, const EnvStateConstPtr state, const std::vector<std::string> &active_links, bool continuous) const
{

  for (std::pair<std::string, COWConstPtr> element : robot_link2cow_)
  {
    COWPtr new_cow(new COW(*(element.second.get())));
    assert(new_cow->getCollisionShape());

    new_cow->setWorldTransform(convertEigenToBt(state->transforms.find(element.first)->second));

    // For descrete checks we can check static to kinematic and kinematic to kinematic
    new_cow->m_collisionFilterGroup = (!active_links.empty() && (std::find_if(active_links.begin(), active_links.end(), [&](std::string link) { return link == element.first; }) == active_links.end())) ? btBroadphaseProxy::StaticFilter : btBroadphaseProxy::KinematicFilter;
    if (new_cow->m_collisionFilterGroup == btBroadphaseProxy::StaticFilter)
    {
      new_cow->m_collisionFilterMask = btBroadphaseProxy::KinematicFilter;
    }
    else
    {
      active_objects.push_back(element.first);
      (continuous) ? (new_cow->m_collisionFilterMask = btBroadphaseProxy::StaticFilter) : (new_cow->m_collisionFilterMask = btBroadphaseProxy::StaticFilter | btBroadphaseProxy::KinematicFilter);
    }

    setContactDistance(new_cow, contact_distance);
    collision_objects[element.first] = new_cow;
  }

  for (std::pair<std::string, COWConstPtr> element : attached_link2cow_)
  {
    COWPtr new_cow(new COW(*(element.second.get())));
    assert(new_cow->getCollisionShape());

    const std::string &parent_link_name = element.second->ptr.m_ab->info.parent_link_name;
    new_cow->setWorldTransform(convertEigenToBt(state->transforms.find(parent_link_name)->second));

    // For descrete checks we can check static to kinematic and kinematic to kinematic
    new_cow->m_collisionFilterGroup = (!active_links.empty() && (std::find_if(active_links.begin(), active_links.end(), [&](std::string link) { return link == parent_link_name; }) == active_links.end())) ? btBroadphaseProxy::StaticFilter : btBroadphaseProxy::KinematicFilter;
    if (new_cow->m_collisionFilterGroup == btBroadphaseProxy::StaticFilter)
    {
      new_cow->m_collisionFilterMask = btBroadphaseProxy::KinematicFilter;
    }
    else
    {
      active_objects.push_back(element.first);
      (continuous) ? (new_cow->m_collisionFilterMask = btBroadphaseProxy::StaticFilter) : (new_cow->m_collisionFilterMask = btBroadphaseProxy::StaticFilter | btBroadphaseProxy::KinematicFilter);
    }

    setContactDistance(new_cow, contact_distance);
    collision_objects[element.first] = new_cow;
  }
}

void BulletEnv::constructBulletObject(Link2Cow& collision_objects,
                                         std::vector<std::string> &active_objects,
                                         double contact_distance,
                                         const EnvStateConstPtr state1,
                                         const EnvStateConstPtr state2,
                                         const std::vector<std::string> &active_links) const
{
  for (std::pair<std::string, COWConstPtr> element : robot_link2cow_)
  {
    COWPtr new_cow(new COW(*(element.second.get())));

    new_cow->m_collisionFilterGroup = (!active_links.empty() && (std::find_if(active_links.begin(), active_links.end(), [&](std::string link) { return link == element.first; }) == active_links.end())) ? btBroadphaseProxy::StaticFilter : btBroadphaseProxy::KinematicFilter;

    if (new_cow->m_collisionFilterGroup == btBroadphaseProxy::StaticFilter)
    {
      new_cow->setWorldTransform(convertEigenToBt(state1->transforms.find(element.first)->second));
      new_cow->m_collisionFilterMask = btBroadphaseProxy::KinematicFilter;
    }
    else
    {
      active_objects.push_back(element.first);

      if (btBroadphaseProxy::isConvex(new_cow->getCollisionShape()->getShapeType()))
      {
        btConvexShape* convex = static_cast<btConvexShape*>(new_cow->getCollisionShape());
        assert(convex != NULL);

        btTransform tf1 = convertEigenToBt(state1->transforms.find(element.first)->second);
        btTransform tf2 = convertEigenToBt(state2->transforms.find(element.first)->second);

        CastHullShape* shape = new CastHullShape(convex, tf1.inverseTimes(tf2));
        assert(shape != NULL);

        new_cow->manage(shape);
        new_cow->setCollisionShape(shape);
        new_cow->setWorldTransform(tf1);
      }
      else if (btBroadphaseProxy::isCompound(new_cow->getCollisionShape()->getShapeType()))
      {
        btCompoundShape* compound = static_cast<btCompoundShape*>(new_cow->getCollisionShape());
        const Eigen::Affine3d &tf1 = state1->transforms.find(element.first)->second;
        const Eigen::Affine3d &tf2 = state2->transforms.find(element.first)->second;

        btCompoundShape* new_compound = new btCompoundShape(/*dynamicAABBtree=*/false);

        for (int i = 0; i < compound->getNumChildShapes(); ++i)
        {
          btConvexShape* convex = static_cast<btConvexShape*>(compound->getChildShape(i));
          assert(convex != NULL);

          btTransform geomTrans = compound->getChildTransform(i);
          btTransform child_tf1 = convertEigenToBt(tf1) * geomTrans;
          btTransform child_tf2 = convertEigenToBt(tf2) * geomTrans;

          btCollisionShape* subshape = new CastHullShape(convex, child_tf1.inverseTimes(child_tf2));
          assert(subshape != NULL);

          if (subshape != NULL)
          {
            new_cow->manage(subshape);
            subshape->setMargin(BULLET_MARGIN);
            new_compound->addChildShape(geomTrans, subshape);
          }
        }

        new_compound->setMargin(BULLET_MARGIN); //margin: compound. seems to have no effect when positive but has an effect when negative
        new_cow->manage(new_compound);
        new_cow->setCollisionShape(new_compound);
        new_cow->setWorldTransform(convertEigenToBt(tf1));
      }
      else
      {
        ROS_ERROR("I can only continuous collision check convex shapes and compound shapes made of convex shapes");
      }

      new_cow->m_collisionFilterMask = btBroadphaseProxy::StaticFilter;
    }

    setContactDistance(new_cow, contact_distance);
    collision_objects[element.first] = new_cow;
  }

  for (std::pair<std::string, COWConstPtr> element : attached_link2cow_)
  {
    COWPtr new_cow(new COW(*(element.second.get())));

    const std::string &parent_link_name = element.second->ptr.m_ab->info.parent_link_name;
    new_cow->m_collisionFilterGroup = (!active_links.empty() && (std::find_if(active_links.begin(), active_links.end(), [&](std::string link) { return link == parent_link_name; }) == active_links.end())) ? btBroadphaseProxy::StaticFilter : btBroadphaseProxy::KinematicFilter;

    if (new_cow->m_collisionFilterGroup == btBroadphaseProxy::StaticFilter)
    {
      new_cow->setWorldTransform(convertEigenToBt(state1->transforms.find(parent_link_name)->second));
      new_cow->m_collisionFilterMask = btBroadphaseProxy::KinematicFilter;
    }
    else
    {
      active_objects.push_back(element.first);

      if (btBroadphaseProxy::isConvex(new_cow->getCollisionShape()->getShapeType()))
      {
        btConvexShape* convex = static_cast<btConvexShape*>(new_cow->getCollisionShape());
        assert(convex != NULL);

        btTransform tf1 = convertEigenToBt(state1->transforms.find(parent_link_name)->second);
        btTransform tf2 = convertEigenToBt(state2->transforms.find(parent_link_name)->second);

        CastHullShape* shape = new CastHullShape(convex, tf1.inverseTimes(tf2));
        assert(shape != NULL);

        new_cow->manage(shape);
        new_cow->setCollisionShape(shape);
        new_cow->setWorldTransform(tf1);
      }
      else if (btBroadphaseProxy::isCompound(new_cow->getCollisionShape()->getShapeType()))
      {
        btCompoundShape* compound = static_cast<btCompoundShape*>(new_cow->getCollisionShape());
        const Eigen::Affine3d &tf1 = state1->transforms.find(parent_link_name)->second;
        const Eigen::Affine3d &tf2 = state2->transforms.find(parent_link_name)->second;

        btCompoundShape* new_compound = new btCompoundShape(/*dynamicAABBtree=*/false);

        for (int i = 0; i < compound->getNumChildShapes(); ++i)
        {
          btConvexShape* convex = static_cast<btConvexShape*>(compound->getChildShape(i));
          assert(convex != NULL);

          btTransform geomTrans = compound->getChildTransform(i);
          btTransform child_tf1 = convertEigenToBt(tf1) * geomTrans;
          btTransform child_tf2 = convertEigenToBt(tf2) * geomTrans;

          btCollisionShape* subshape = new CastHullShape(convex, child_tf1.inverseTimes(child_tf2));
          assert(subshape != NULL);

          if (subshape != NULL)
          {
            new_cow->manage(subshape);
            subshape->setMargin(BULLET_MARGIN);
            new_compound->addChildShape(geomTrans, subshape);
          }
        }

        new_compound->setMargin(BULLET_MARGIN); //margin: compound. seems to have no effect when positive but has an effect when negative
        new_cow->manage(new_compound);
        new_cow->setCollisionShape(new_compound);
        new_cow->setWorldTransform(convertEigenToBt(tf1));
      }
      else
      {
        ROS_ERROR("I can only continuous collision check convex shapes and compound shapes made of convex shapes");
      }

      new_cow->m_collisionFilterMask = btBroadphaseProxy::StaticFilter;
    }

    setContactDistance(new_cow, contact_distance);
    collision_objects[element.first] = new_cow;
  }
}

moveit_msgs::RobotStatePtr BulletEnv::getRobotStateMsg() const
{
  moveit_msgs::RobotStatePtr msg(new moveit_msgs::RobotState());
  msg->is_diff = false;
  msg->joint_state.name.reserve(current_state_->joints.size());
  msg->joint_state.position.reserve(current_state_->joints.size());
  for (const auto& joint : current_state_->joints)
  {
    msg->joint_state.name.push_back(joint.first);
    msg->joint_state.position.push_back(joint.second);
  }

  for (const auto& body : attached_bodies_)
  {
    moveit_msgs::AttachedCollisionObject obj;
    obj.link_name = body.second->info.parent_link_name;
    obj.touch_links = body.second->info.touch_links;

    obj.object.id = body.second->obj->name;
    obj.object.header.frame_id = body.second->info.parent_link_name;
    obj.object.header.stamp = ros::Time::now();

    for (auto i = 0; i < body.second->obj->shapes.size(); ++i)
    {
      const auto geom = body.second->obj->shapes[i];
      const auto geom_pose = body.second->obj->shapes_trans[i];
      if (geom->type == shapes::OCTREE)
      {
        const shapes::OcTree* g = static_cast<const shapes::OcTree*>(geom.get());
        double occupancy_threshold = g->octree->getOccupancyThres();

        for(auto it = g->octree->begin(g->octree->getTreeDepth()), end = g->octree->end(); it != end; ++it)
        {
          if(it->getOccupancy() >= occupancy_threshold)
          {
            double size = it.getSize();
            shape_msgs::SolidPrimitive s;
            s.type = shape_msgs::SolidPrimitive::BOX;
            s.dimensions.resize(3);
            s.dimensions[shape_msgs::SolidPrimitive::BOX_X] = size;
            s.dimensions[shape_msgs::SolidPrimitive::BOX_Y] = size;
            s.dimensions[shape_msgs::SolidPrimitive::BOX_Z] = size;
            obj.object.primitives.push_back(s);

            Eigen::Affine3d trans, final_trans;
            trans.setIdentity();
            trans.translation() = Eigen::Vector3d(it.getX(), it.getY(), it.getZ());
            final_trans = geom_pose * trans;

            geometry_msgs::Pose pose;
            tf::poseEigenToMsg(final_trans, pose);
            obj.object.primitive_poses.push_back(pose);
          }
        }
      }
      else if (geom->type == shapes::MESH)
      {
        shapes::ShapeMsg s;
        shapes::constructMsgFromShape(geom.get(), s);

        obj.object.meshes.push_back(boost::get<shape_msgs::Mesh>(s));

        geometry_msgs::Pose pose;
        tf::poseEigenToMsg(geom_pose, pose);
        obj.object.mesh_poses.push_back(pose);
      }
      else if (geom->type == shapes::PLANE)
      {
        shapes::ShapeMsg s;
        shapes::constructMsgFromShape(geom.get(), s);
        obj.object.planes.push_back(boost::get<shape_msgs::Plane>(s));

        geometry_msgs::Pose pose;
        tf::poseEigenToMsg(geom_pose, pose);
        obj.object.plane_poses.push_back(pose);
      }
      else //SolidPrimitive
      {
        shapes::ShapeMsg s;
        shapes::constructMsgFromShape(geom.get(), s);
        obj.object.primitives.push_back(boost::get<shape_msgs::SolidPrimitive>(s));

        geometry_msgs::Pose pose;
        tf::poseEigenToMsg(geom_pose, pose);
        obj.object.primitive_poses.push_back(pose);
      }
    }
    msg->attached_collision_objects.push_back(obj);
  }
  return msg;
}

void BulletEnv::updateVisualization() const
{
  moveit_msgs::DisplayRobotState msg;

  msg.state = *getRobotStateMsg();
  scene_pub_.publish(msg);
}

void BulletEnv::plotTrajectory(const std::string &name, const std::vector<std::string> &joint_names, const TrajArray &traj)
{
  moveit_msgs::DisplayTrajectory msg;
  moveit_msgs::RobotTrajectory rt;

  // Set the Robot State so attached objects show up
  msg.trajectory_start = *getRobotStateMsg();

  // Initialze the whole traject with the current state.
  rt.joint_trajectory.joint_names.resize(joint_to_qnr_.size());
  rt.joint_trajectory.points.resize(traj.rows());
  for (int i = 0; i < traj.rows(); ++i)
  {
    trajectory_msgs::JointTrajectoryPoint jtp;
    jtp.positions.resize(joint_to_qnr_.size());
    for (const auto& it : joint_to_qnr_)
    {
      if (i == 0)
      {
        rt.joint_trajectory.joint_names[it.second] = it.first;
      }
      jtp.positions[it.second] = kdl_jnt_array_(it.second);
    }
    jtp.time_from_start = ros::Duration(i);
    rt.joint_trajectory.points[i] = jtp;
  }

  // Update only the joints which were provided.
  for (int i = 0; i < traj.rows(); ++i)
  {
    for (int j = 0; j < traj.cols(); ++j)
    {
      rt.joint_trajectory.points[i].positions[joint_to_qnr_[joint_names[j]]] = traj(i, j);
    }
  }
  msg.trajectory.push_back(rt);
  trajectory_pub_.publish(msg);
}

visualization_msgs::Marker BulletEnv::getMarkerArrowMsg(const Eigen::Vector3d &pt1, const Eigen::Vector3d &pt2, const Eigen::Vector4d &rgba, double scale)
{
  visualization_msgs::Marker marker;
  marker.header.frame_id = model_->getRoot()->name;
  marker.header.stamp = ros::Time::now();
  marker.ns = "trajopt";
  marker.id = ++marker_counter_;
  marker.type = visualization_msgs::Marker::ARROW;
  marker.action = visualization_msgs::Marker::ADD;

  Eigen::Vector3d x, y, z;
  x = (pt2 - pt1).normalized();
  marker.pose.position.x = pt1(0);
  marker.pose.position.y = pt1(1);
  marker.pose.position.z = pt1(2);

  y = x.unitOrthogonal();
  z = (x.cross(y)).normalized();
  Eigen::Matrix3d rot;
  rot.col(0) = x;
  rot.col(1) = y;
  rot.col(2) = z;
  Eigen::Quaterniond q(rot);
  q.normalize();
  marker.pose.orientation.x = q.x();
  marker.pose.orientation.y = q.y();
  marker.pose.orientation.z = q.z();
  marker.pose.orientation.w = q.w();

  marker.scale.x = std::abs((pt2 - pt1).norm());
  marker.scale.y = scale;
  marker.scale.z = scale;

  marker.color.r = rgba(0);
  marker.color.g = rgba(1);
  marker.color.b = rgba(2);
  marker.color.a = rgba(3);

  return marker;
}

visualization_msgs::Marker BulletEnv::getMarkerCylinderMsg(const Eigen::Vector3d &pt1, const Eigen::Vector3d &pt2, const Eigen::Vector4d &rgba, double scale)
{
  visualization_msgs::Marker marker;
  marker.header.frame_id = model_->getRoot()->name;
  marker.header.stamp = ros::Time::now();
  marker.ns = "trajopt";
  marker.id = ++marker_counter_;
  marker.type = visualization_msgs::Marker::CYLINDER;
  marker.action = visualization_msgs::Marker::ADD;

  Eigen::Vector3d x, y, z;
  x = (pt2 - pt1).normalized();
  marker.pose.position.x = pt1(0);
  marker.pose.position.y = pt1(1);
  marker.pose.position.z = pt1(2);

  y = x.unitOrthogonal();
  z = (x.cross(y)).normalized();
  Eigen::Matrix3d rot;
  rot.col(0) = x;
  rot.col(1) = y;
  rot.col(2) = z;
  Eigen::Quaterniond q(rot);
  q.normalize();
  marker.pose.orientation.x = q.x();
  marker.pose.orientation.y = q.y();
  marker.pose.orientation.z = q.z();
  marker.pose.orientation.w = q.w();

  double length = std::abs((pt2 - pt1).norm());
  marker.scale.x = scale * length/20.0;
  marker.scale.y = scale * length/20.0;
  marker.scale.z = scale * length;

  marker.color.r = rgba(0);
  marker.color.g = rgba(1);
  marker.color.b = rgba(2);
  marker.color.a = rgba(3);

  return marker;
}

void BulletEnv::plotArrow(const Eigen::Vector3d &pt1, const Eigen::Vector3d &pt2, const Eigen::Vector4d &rgba, double scale)
{
  visualization_msgs::MarkerArray msg;
  msg.markers.push_back(getMarkerArrowMsg(pt1, pt2, rgba, scale));
  arrows_pub_.publish(msg);
}

void BulletEnv::plotAxis(const Eigen::Affine3d &axis, double scale)
{
  visualization_msgs::MarkerArray msg;
  Eigen::Vector3d x_axis = axis.matrix().block<3, 1>(0, 0);
  Eigen::Vector3d y_axis = axis.matrix().block<3, 1>(0, 1);
  Eigen::Vector3d z_axis = axis.matrix().block<3, 1>(0, 2);
  Eigen::Vector3d position = axis.matrix().block<3, 1>(0, 3);

  msg.markers.push_back(getMarkerCylinderMsg(position, position + 0.1 * x_axis, Eigen::Vector4d(1, 0, 0, 1), scale));
  msg.markers.push_back(getMarkerCylinderMsg(position, position + 0.1 * y_axis, Eigen::Vector4d(0, 1, 0, 1), scale));
  msg.markers.push_back(getMarkerCylinderMsg(position, position + 0.1 * z_axis, Eigen::Vector4d(0, 0, 1, 1), scale));
  axes_pub_.publish(msg);
}

void BulletEnv::plotCollisions(const std::vector<std::string> &link_names, const DistanceResultVector &dist_results, double safe_dist)
{
  visualization_msgs::MarkerArray msg;
  for (int i = 0; i < dist_results.size(); ++i)
  {
    const DistanceResult &dist = dist_results[i];

    if (!dist.valid)
      continue;

    Eigen::Vector4d rgba;
    if (dist.distance < 0)
    {
      rgba << 1.0, 0.0, 0.0, 1.0;
    }
    else if (dist.distance < safe_dist)
    {
      rgba << 1.0, 1.0, 0.0, 1.0;
    }
    else
    {
      rgba << 0.0, 1.0, 0.0, 1.0;
    }

    Eigen::Vector3d ptA, ptB;
    ptA = dist.nearest_points[0];
    ptB = dist.nearest_points[1];

    auto it = std::find(link_names.begin(), link_names.end(), dist.link_names[0]);
    if (it != link_names.end())
    {
      ptA = dist.nearest_points[1];
      ptB = dist.nearest_points[0];
    }

    if(dist.cc_type == ContinouseCollisionType::CCType_Between)
    {
      Eigen::Vector4d cc_rgba;
      cc_rgba << 0.0, 0.0, 0.0, 1.0;
      msg.markers.push_back(getMarkerArrowMsg(ptB, dist.cc_nearest_points[1], cc_rgba, 0.01));

      // DEGUG: This was added to see what the original contact point was for the cast continuous
      //        collision checking. Should be removed as everything has been integrated and tested.
      Eigen::Vector4d temp_rgba;
      temp_rgba << 0.0, 0.0, 1.0, 1.0;
      msg.markers.push_back(getMarkerArrowMsg(ptA, dist.cc_nearest_points[0], temp_rgba, 0.01));

      ptB = ((1 - dist.cc_time) * ptB + dist.cc_time * dist.cc_nearest_points[1]);

    }

     msg.markers.push_back(getMarkerArrowMsg(ptA, ptB, rgba, 0.01));
  }

  if (dist_results.size() > 0)
  {
    collisions_pub_.publish(msg);
  }
}

void BulletEnv::plotClear()
{
  // Remove old arrows
  marker_counter_ = 0;
  visualization_msgs::MarkerArray msg;
  visualization_msgs::Marker marker;
  marker.header.frame_id = model_->getRoot()->name;
  marker.header.stamp = ros::Time();
  marker.ns = "trajopt";
  marker.id = 0;
  marker.type = visualization_msgs::Marker::ARROW;
  marker.action = visualization_msgs::Marker::DELETEALL;
  msg.markers.push_back(marker);
  collisions_pub_.publish(msg);
  arrows_pub_.publish(msg);
  axes_pub_.publish(msg);

  ros::Duration(0.5).sleep();
}

void BulletEnv::plotWaitForInput()
{
  ROS_ERROR("Hit enter key to step optimization!");
  std::cin.ignore(std::numeric_limits<std::streamsize>::max(),'\n');
}

}
