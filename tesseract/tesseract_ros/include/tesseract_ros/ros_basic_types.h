/**
 * @file ros_basic_types.h
 * @brief Tesseract ROS Basic types.
 *
 * @author Levi Armstrong
 * @date April 15, 2018
 * @version TODO
 * @bug No known bugs
 *
 * @copyright Copyright (c) 2013, Southwest Research Institute
 *
 * @par License
 * Software License Agreement (Apache License)
 * @par
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 * http://www.apache.org/licenses/LICENSE-2.0
 * @par
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef TESSERACT_ROS_BASIC_TYPES_H
#define TESSERACT_ROS_BASIC_TYPES_H

#include <unordered_map>
#include <string>
#include <vector>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <geometric_shapes/shape_operations.h>
#include <tesseract_core/basic_types.h>

namespace tesseract
{

namespace tesseract_ros
{

struct ROSAllowedCollisionMatrix : public AllowedCollisionMatrix
{
  /**
   * @brief Disable collision between two collision objects
   * @param obj1 Collision object name
   * @param obj2 Collision object name
   * @param reason The reason for disabling collison
   */
  virtual void addAllowedCollision(const std::string &link_name1, const std::string &link_name2, const std::string &reason)
  {
    lookup_table_[link_name1 + link_name2] = reason;
    lookup_table_[link_name2 + link_name1] = reason;
  }

  /**
   * @brief Remove disabled collision pair from allowed collision matrix
   * @param obj1 Collision object name
   * @param obj2 Collision object name
   */
  virtual void removeAllowedCollision(const std::string &link_name1, const std::string &link_name2)
  {
    lookup_table_.erase(link_name1 + link_name2);
    lookup_table_.erase(link_name2 + link_name1);
  }

  bool isCollisionAllowed(const std::string &link_name1, const std::string &link_name2) const
  {
    return (lookup_table_.find(link_name1 + link_name2) != lookup_table_.end());
  }

private:
  std::unordered_map<std::string, std::string> lookup_table_;
};
typedef std::shared_ptr<ROSAllowedCollisionMatrix> ROSAllowedCollisionMatrixPtr;
typedef std::shared_ptr<const ROSAllowedCollisionMatrix> ROSAllowedCollisionMatrixConstPtr;

/** @brief This holds a state of the environment */
struct EnvState
{
  std::unordered_map<std::string, double> joints;
  std::unordered_map<std::string, Eigen::Affine3d> transforms;
};
typedef std::shared_ptr<EnvState> EnvStatePtr;
typedef std::shared_ptr<const EnvState> EnvStateConstPtr;

/**< @brief Information on how the object is attached to the environment */
struct AttachedBodyInfo
{
  std::string name;                     /**< @brief The name of the attached body (must be unique) */
  std::string parent_link_name;         /**< @brief The name of the link to attach the body */
  std::string object_name;              /**< @brief The name of the AttachableObject being used */
  std::vector<std::string> touch_links; /**< @brief The names of links which the attached body is allowed to be in contact with */
};

/** @brief Contains geometry data for an attachable object */
struct AttachableObjectGeometry
{
  std::vector<shapes::ShapeConstPtr> shapes;  /**< @brief The shape */
  EigenSTL::vector_Affine3d shape_poses;      /**< @brief The pose of the shape */
  EigenSTL::vector_Vector4d shape_colors;     /**< @brief (Optional) The shape color (R, G, B, A) */
};

/** @brief Contains data about an attachable object */
struct AttachableObject
{
  std::string name;                   /**< @brief The name of the attachable object */
  AttachableObjectGeometry visual;    /**< @brief The objects visual geometry */
  AttachableObjectGeometry collision; /**< @brief The objects collision geometry */
};
typedef std::shared_ptr<AttachableObject> AttachableObjectPtr;
typedef std::shared_ptr<const AttachableObject> AttachableObjectConstPtr;

/** @brief Contains data representing an attached body */
struct AttachedBody
{
   AttachedBodyInfo info;        /**< @brief Information on how the object is attached to the environment */
   AttachableObjectConstPtr obj; /**< @brief The attached bodies object data */
};
typedef std::shared_ptr<AttachedBody> AttachedBodyPtr;
typedef std::shared_ptr<const AttachedBody> AttachedBodyConstPtr;

/** @brief ObjectColorMap Stores Object color in a 4d vector as RGBA*/
struct ObjectColor
{
  EigenSTL::vector_Vector4d visual;
  EigenSTL::vector_Vector4d collision;
};
typedef std::unordered_map<std::string, ObjectColor> ObjectColorMap;
typedef std::shared_ptr<ObjectColorMap> ObjectColorMapPtr;
typedef std::shared_ptr<const ObjectColorMap> ObjectColorMapConstPtr;
typedef std::unordered_map<std::string, AttachedBodyConstPtr> AttachedBodyConstPtrMap;
typedef std::unordered_map<std::string, AttachableObjectConstPtr> AttachableObjectConstPtrMap;
}
}
#endif // TESSERACT_ROS_BASIC_TYPES_H
