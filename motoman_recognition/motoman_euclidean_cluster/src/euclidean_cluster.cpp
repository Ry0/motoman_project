// #include <euclidean_cluster.hpp>
#include "../include/euclidean_cluster.hpp"

using namespace pcl;

EuclideanCluster::EuclideanCluster(ros::NodeHandle nh, ros::NodeHandle n)
    : nh_(nh),
      rate_(n.param("loop_rate", 10)),
      frame_id_(n.param<std::string>("clustering_frame_id", "world"))
{
  source_pc_sub_ = nh_.subscribe(n.param<std::string>("source_pc_topic_name", "/merged_cloud"), 1, &EuclideanCluster::EuclideanCallback, this);
  euclidean_cluster_pub_ = nh_.advertise<jsk_recognition_msgs::BoundingBoxArray>(n.param<std::string>("box_name", "/clustering_result"), 1);

  // クラスタリングのパラメータを初期化
  n.param<double>("clusterTolerance", clusterTolerance_, 0.02);
  n.param<int>("minSize", minSize_, 100);
  n.param<int>("maxSize", maxSize_, 2500);
  // clopboxを当てはめるエリアを定義
  n.param<float>("crop_x_min", crop_min_.x, 0.15);
  n.param<float>("crop_x_max", crop_max_.x, 1.5);
  n.param<float>("crop_y_min", crop_min_.y, -1.5);
  n.param<float>("crop_y_max", crop_max_.y, 1.5);
  n.param<float>("crop_z_min", crop_min_.z, 0.01);
  n.param<float>("crop_z_max", crop_max_.z, 0.5);
}

void EuclideanCluster::EuclideanCallback(
    const sensor_msgs::PointCloud2::ConstPtr &source_pc) {

  //点群をKinect座標系からWorld座標系に変換
  //変換されたデータはtrans_pcに格納される．
  sensor_msgs::PointCloud2 trans_pc;
  try {
    pcl_ros::transformPointCloud(frame_id_, *source_pc, trans_pc, tf_);
  } catch (tf::ExtrapolationException e) {
    ROS_ERROR("pcl_ros::transformPointCloud %s", e.what());
  }

  // sensor_msgs::PointCloud2 → pcl::PointCloud
  pcl::PointCloud<PointXYZ> pcl_source;
  pcl::fromROSMsg(trans_pc, pcl_source);
  pcl::PointCloud<PointXYZ>::Ptr pcl_source_ptr(new pcl::PointCloud<PointXYZ>(pcl_source));

  // 点群の中からnanを消す
  // std::vector<int> dummy;
  // pcl::removeNaNFromPointCloud(*pcl_source_ptr, *pcl_source_ptr, dummy);

  // 平面をしきい値で除去する→Cropboxで
  CropBox(pcl_source_ptr, crop_min_, crop_max_);

  // Creating the KdTree object for the search method of the extraction
  Clustering(pcl_source_ptr);
}

void EuclideanCluster::CropBox(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud,
                               pcl::PointXYZ min, pcl::PointXYZ max) {
  Eigen::Vector4f minPoint;

  minPoint[0] = min.x; // define minimum point x
  minPoint[1] = min.y; // define minimum point y
  minPoint[2] = min.z; // define minimum point z

  Eigen::Vector4f maxPoint;
  maxPoint[0] = max.x; // define max point x
  maxPoint[1] = max.y; // define max point y
  maxPoint[2] = max.z; // define max point z

  Eigen::Vector3f boxTranslatation;
  boxTranslatation[0] = 0;
  boxTranslatation[1] = 0;
  boxTranslatation[2] = 0;

  Eigen::Vector3f boxRotation;
  boxRotation[0] = 0; // rotation around x-axis
  boxRotation[1] = 0; // rotation around y-axis
  boxRotation[2] = 0; // in radians rotation around z-axis. this rotates your

  Eigen::Affine3f boxTransform;

  pcl::CropBox<pcl::PointXYZ> cropFilter;
  cropFilter.setInputCloud(cloud);
  cropFilter.setMin(minPoint);
  cropFilter.setMax(maxPoint);
  cropFilter.setTranslation(boxTranslatation);
  cropFilter.setRotation(boxRotation);

  pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_filtered(new pcl::PointCloud<pcl::PointXYZ>());
  cropFilter.filter(*cloud_filtered);
  pcl::copyPointCloud<pcl::PointXYZ, pcl::PointXYZ>(*cloud_filtered, *cloud);
}

void EuclideanCluster::Clustering(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud) {
  pcl::search::KdTree<pcl::PointXYZ>::Ptr tree(new pcl::search::KdTree<pcl::PointXYZ>);
  tree->setInputCloud(cloud);

  std::vector<pcl::PointIndices> cluster_indices;
  pcl::EuclideanClusterExtraction<pcl::PointXYZ> ec;
  ec.setClusterTolerance(0.02); // 2cm
  ec.setMinClusterSize(100);
  ec.setMaxClusterSize(25000);
  ec.setSearchMethod(tree);
  ec.setInputCloud(cloud);
  ec.extract(cluster_indices);

  int j = 0;
  jsk_recognition_msgs::BoundingBoxArray box_array; // clustering結果をぶち込む配列

  for (std::vector<pcl::PointIndices>::const_iterator it = cluster_indices.begin(); it != cluster_indices.end(); ++it) {
    pcl::PointCloud<pcl::PointXYZ>::Ptr cloud_cluster(new pcl::PointCloud<pcl::PointXYZ>);
    for (std::vector<int>::const_iterator pit = it->indices.begin(); pit != it->indices.end(); ++pit)
      cloud_cluster->points.push_back(cloud->points[*pit]);

    cloud_cluster->width = cloud_cluster->points.size();
    cloud_cluster->height = 1;
    cloud_cluster->is_dense = true;

    // 一つのclusterをpushback
    jsk_recognition_msgs::BoundingBox box;
    box = MomentOfInertia_AABB(cloud_cluster, j);
    box_array.boxes.push_back(box);

    j++;
  }

  // int clusterLength = clusterIndices.size();
  ROS_INFO("Found %lu clusters:", cluster_indices.size());

  // publish
  box_array.header.stamp = ros::Time::now();
  box_array.header.frame_id = frame_id_;
  euclidean_cluster_pub_.publish(box_array);

  // Empty Buffer
  cluster_indices.clear();
}

jsk_recognition_msgs::BoundingBox EuclideanCluster::MomentOfInertia_AABB(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud, int cluster_cnt) {
  pcl::MomentOfInertiaEstimation<pcl::PointXYZ> feature_extractor;
  feature_extractor.setInputCloud(cloud);
  feature_extractor.compute();

  std::vector<float> moment_of_inertia;
  std::vector<float> eccentricity;
  pcl::PointXYZ min_point_AABB;
  pcl::PointXYZ max_point_AABB;

  geometry_msgs::Pose pose;
  geometry_msgs::Vector3 size;

  feature_extractor.getMomentOfInertia(moment_of_inertia);
  feature_extractor.getEccentricity(eccentricity);
  feature_extractor.getAABB(min_point_AABB, max_point_AABB);

  pose.position.x = (min_point_AABB.x + max_point_AABB.x) / 2.0;
  pose.position.y = (min_point_AABB.y + max_point_AABB.y) / 2.0;
  pose.position.z = (min_point_AABB.z + max_point_AABB.z) / 2.0;
  // std::cout << pose.position.x << ", " << pose.position.y << ", " << pose.position.z << std::endl;

  size.x = max_point_AABB.x - min_point_AABB.x;
  size.y = max_point_AABB.y - min_point_AABB.y;
  size.z = max_point_AABB.z - min_point_AABB.z;
  // std::cout << size.x << ", " << size.y << ", " << size.z << std::endl;
  // std::cout << std::endl;

  // TFの名前付け
  std::stringstream ss;
  std::string object_name;
  ss << cluster_cnt;
  object_name = "object_" + ss.str();

  br_.sendTransform(tf::StampedTransform(
      tf::Transform(
          tf::Quaternion(0, 0, 0, 1),
          tf::Vector3(pose.position.x, pose.position.y, max_point_AABB.z)),
          ros::Time::now(), "world", object_name));

  jsk_recognition_msgs::BoundingBox box;
  box.header.frame_id = frame_id_;
  box.pose = pose;
  box.dimensions = size;
  // box.label = cluster_cnt;

  return box;
}

jsk_recognition_msgs::BoundingBox EuclideanCluster::MomentOfInertia_OBB(pcl::PointCloud<pcl::PointXYZ>::Ptr cloud) {
  pcl::MomentOfInertiaEstimation<pcl::PointXYZ> feature_extractor;
  feature_extractor.setInputCloud(cloud);
  feature_extractor.compute();

  std::vector<float> moment_of_inertia;
  std::vector<float> eccentricity;
  pcl::PointXYZ min_point_OBB;
  pcl::PointXYZ max_point_OBB;
  pcl::PointXYZ position_OBB;
  Eigen::Matrix3f rotational_matrix_OBB;
  float major_value, middle_value, minor_value;
  Eigen::Vector3f major_vector, middle_vector, minor_vector;
  Eigen::Vector3f mass_center;

  geometry_msgs::Pose pose;
  geometry_msgs::Vector3 size;

  feature_extractor.getMomentOfInertia(moment_of_inertia);
  feature_extractor.getEccentricity(eccentricity);
  feature_extractor.getOBB(min_point_OBB, max_point_OBB, position_OBB, rotational_matrix_OBB);
  feature_extractor.getEigenValues(major_value, middle_value, minor_value);
  feature_extractor.getEigenVectors(major_vector, middle_vector, minor_vector);
  feature_extractor.getMassCenter(mass_center);
  Eigen::Quaternionf quat(rotational_matrix_OBB);

  pose.position.x = mass_center(0);
  pose.position.y = mass_center(1);
  pose.position.z = mass_center(2);
  pose.orientation.x = quat.x();
  pose.orientation.y = quat.y();
  pose.orientation.z = quat.z();
  pose.orientation.w = quat.w();
  // std::cout << pose.position.x << ", " << pose.position.y << ", " << pose.position.z << std::endl;

  size.x = max_point_OBB.x - min_point_OBB.x;
  size.y = max_point_OBB.y - min_point_OBB.y;
  size.z = max_point_OBB.z - min_point_OBB.z;
  // std::cout << size.x << ", " << size.y << ", " << size.z << std::endl;
  // std::cout << std::endl;

  jsk_recognition_msgs::BoundingBox box;
  box.header.frame_id = frame_id_;
  box.pose = pose;
  box.dimensions = size;

  return box;
}

void EuclideanCluster::run()
{
  while(nh_.ok()){
    ros::spinOnce();
    rate_.sleep();
  }
}
