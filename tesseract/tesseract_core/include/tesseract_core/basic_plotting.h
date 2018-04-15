#ifndef BASIC_PLOTTER_H
#define BASIC_PLOTTER_H

#include <tesseract_core/basic_types.h>

namespace tesseract
{

/** @brief The BasicPlotting class */
class BasicPlotting
{
public:

  /**
   * @brief Plot a trajectory
   * @param traj
   */
  virtual void plotTrajectory(const std::vector<std::string> &joint_names, const TrajArray &traj) = 0;

  /**
   * @brief Plot the collision results data
   * @param link_names List of link names for which to plot data
   * @param dist_results The collision results data
   * @param safety_distance Vector of safety Distance corresponding to dist_results (Must be in the same order and length).
   */
  virtual void plotContactResults(const std::vector<std::string> &link_names, const DistanceResultVector &dist_results, const Eigen::VectorXd& safety_distances) = 0;

  /**
   * @brief Plot arrow defined by two points
   * @param pt1 Start position of the arrow
   * @param pt2 Final position of the arrow
   * @param rgba Color of the arrow
   * @param scale The size of the arrow (related to diameter)
   */
  virtual void plotArrow(const Eigen::Vector3d &pt1, const Eigen::Vector3d &pt2, const Eigen::Vector4d &rgba, double scale) = 0;

  /**
   * @brief Plot axis
   * @param axis The axis
   * @param scale The size of the axis
   */
  virtual void plotAxis(const Eigen::Affine3d &axis, double scale) = 0;

  /**
   * @brief This is called at the start of the plotting for each iteration
   *        to clear previous iteration graphics if neccessary.
   */
  virtual void clear() = 0;

  /** @brief Pause code and wait for enter key in terminal*/
  virtual void waitForInput() = 0;
};
typedef boost::shared_ptr<BasicPlotting> BasicPlottingPtr;
typedef boost::shared_ptr<const BasicPlotting> BasicPlottingConstPtr;

}

#endif // BASIC_PLOTTER_H
