#ifndef BASIC_COLL_H
#define BASIC_COLL_H
/**
 * @file basic_coll.h
 * @brief Basic low-level collision and distance functions.
 *
 * @author Levi Armstrong
 * @date Dec 18, 2017
 * @version TODO
 * @bug No known bugs
 *
 * @copyright Copyright (c) 2017, Southwest Research Institute
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
#include <vector>
#include <string>
#include <Eigen/Core>
#include <Eigen/Geometry>
#include <trajopt/common.hpp>

namespace trajopt
{

class TRAJOPT_API BasicColl
{
public:
  EIGEN_MAKE_ALIGNED_OPERATOR_NEW

  BasicColl() : initialized_(false) {}

  /**
   * @brief Initializes BasicColl
   * @return True if init() completes successfully
   */
  virtual bool init() = 0;

  /**
   * @brief calcDistances Should return distance information for all active links
   * @param joint_angles Vector of joint angles (size must match number of joints in robot chain)
   */
  virtual void calcDistances(const Eigen::VectorXd &joint_angles) = 0;

  /**
   * @brief calcDistances Should return distance information for all links in list link_names
   * @param joint_angles Vector of joint angles (size must match number of joints in robot chain)
   * @param link_names Name of the links to calculate distance data for.
   */
  virtual void calcDistances(const Eigen::VectorXd &joint_angles, const std::vector<std::string> &link_names) = 0;

  /**
   * @brief calcCollisions Should return collision information for all active links
   * @param joint_angles Vector of joint angles (size must match number of joints in robot chain)
   */
  virtual void calcCollisions(const Eigen::VectorXd &joint_angles) = 0;

  /**
   * @brief calcCollisions Should return collision information for all links in list link_names
   * @param joint_angles Vector of joint angles (size must match number of joints in robot chain)
   * @param link_names Name of the links to calculate collision data for.
   */
  virtual void calcCollisions(const Eigen::VectorXd &joint_angles, const std::vector<std::string> &link_names) = 0;

private:
  bool initialized_; /**< Identifies if the object has been initialized */

}; // class BasicColl

typedef boost::shared_ptr<BasicColl> BasicCollPtr;
} // namespace trajopt

#endif // BASIC_COLL_H
