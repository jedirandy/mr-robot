﻿#include <ros/ros.h>
#include <visualization_msgs/Marker.h>
#include <geometry_msgs/Twist.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/Image.h>

#include <image_transport/image_transport.h>
#include <cv_bridge/cv_bridge.h>
#include <sensor_msgs/image_encodings.h>
#include <opencv2/imgproc/imgproc.hpp>
#include <opencv2/highgui/highgui.hpp>

#include <std_msgs/Float32.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl/point_types.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>

#include <sys/time.h>
#include <map>
#include <vector>

struct MineDetectorParameters
{
	// Dimensions of the projected image from floor_projector
	double floor_size_pix;
};

static MineDetectorParameters sParameters;

////////////
// Utilities

template <typename T>
T Clip(T v, T lowerBound, T upperBound)
{
	return std::min(std::max(v,lowerBound),upperBound);
}

static double squaredDistance2D(double x1, double y1, double x2, double y2)
{
	double dx=x2-x1;
	double dy=y2-y1;
	return dx*dx+dy*dy;
}

// Bijection from N^2 to N
static int BijectioN2ToN(int x, int y)
{
	return x+(x+y)*(x+y+1)/2;
}

// End of utilities
///////////////////

class Timer
{
private:
	timeval startTime;

public:
	void start()	{gettimeofday(&startTime, NULL);}

	double stop()
	{
		timeval endTime;
		long seconds, useconds;
		double duration;

		gettimeofday(&endTime, NULL);

		seconds  = endTime.tv_sec  - startTime.tv_sec;
		useconds = endTime.tv_usec - startTime.tv_usec;

		duration = seconds + useconds/1000000.0;

		return duration;
	}
};

// The class which does the real job
class MineDetector
{
protected:
	ros::NodeHandle m_NodeHandle;
	ros::Subscriber m_RobotCommandSubscriber;
	ros::Subscriber m_FloorProjectorSubscriber;
	ros::Subscriber m_DepthSensorSubscriber;
	ros::Subscriber m_MetalDetectorSubscriber;
	ros::Publisher m_MineMarkerPublisher;
	ros::Publisher m_ArmTwistPublisher;
	tf::TransformListener m_Listener;

	ros::Time m_MsgHeaderStamp;
	std::vector<tf::Vector3> m_vMinesPositions;

	////////
	// State
	tf::Vector3 m_WorldSpaceRobotPosition;
	tf::Vector3 m_WorldSpaceToolPosition;
	double m_Orientation;

	geometry_msgs::Twist m_RobotSpeed;
	geometry_msgs::Twist m_ArmTipCommand;

	cv_bridge::CvImagePtr m_pProcessedImage;
	// Vector in which the indices correspond to the indices in m_pProcessedImage where the interface was found
	std::map<int,std::pair<int, int>> m_Interface;
	// (row,column) coordinates in pixels
	std::pair<double, double> m_PointToTrack;

protected:
	//===============//
	// ROS Callbacks //

	void RobotCommandCallback(const geometry_msgs::Twist msg)
	{
		m_RobotSpeed = msg;
	}

	void DepthSensorCallback(const sensor_msgs::PointCloud2ConstPtr msg)
	{
		// Get the current world space tool position
		m_MsgHeaderStamp = msg->header.stamp;
		m_Listener.waitForTransform("/world", "VSV/Tool",
			m_MsgHeaderStamp, ros::Duration(1.0));
		tf::StampedTransform transform;
		m_Listener.lookupTransform("/world", "VSV/Tool",
			m_MsgHeaderStamp, transform);
		m_WorldSpaceToolPosition = transform * tf::Vector3(0.0, 0.0, 0.0);

		// Get the current world space robot position
		m_Listener.waitForTransform("/world", "VSV/base",
			m_MsgHeaderStamp, ros::Duration(1.0));
		m_Listener.lookupTransform("/world", "VSV/base",
			m_MsgHeaderStamp, transform);
		m_WorldSpaceRobotPosition = transform * tf::Vector3(0.0, 0.0, 0.0);
		
		//ROS_INFO("Base position %f %f %f",
			//	m_WorldSpaceRobotPosition.x(), m_WorldSpaceRobotPosition.y(), m_WorldSpaceRobotPosition.z());

		// Get the orientation of the robot
		auto Q = transform.getRotation();
		tf::Matrix3x3 M;
		M.setRotation(Q);
		double roll, pitch;
		M.getRPY(roll, pitch, m_Orientation);
	}

	// Detects the change of color in a picture (with a low number of different colors)
	// The result is store in a map in which the indices correspond to the indices in the picture
	// The key is obtained with BijectioN2ToN. We use a map to avoid doobloons
	void DetectInterface(std::map<int,std::pair<int, int>> &out)
	{
		unsigned char data, data_;
		out.clear();
		for( int row = 0; row < m_pProcessedImage->image.rows; ++row )
		{
			for ( int col = 0; col < m_pProcessedImage->image.cols; ++col )
			{

				data = m_pProcessedImage->image.data[m_pProcessedImage->image.rows * row + col];
				if(data!=0)
				{
					out.insert(std::make_pair<int,std::pair<int, int>>(BijectioN2ToN(row,col), std::make_pair(row, col)));
					//ROS_INFO("%d %d", row, col);
				}
			}
		}
	}

	// This function computes the intersection between the middle of the picture
	void ComputePointToTrack()
	{
		int indexToSearch = sParameters.floor_size_pix/2.0;
		std::vector<double> vVerticalPixelCoordinates;
		for(auto it = m_Interface.begin(); it != m_Interface.end(); ++it)
		{
			// it->second.first: index of the pixel row in the projected image from the camera
			if(it->second.first == indexToSearch)
				vVerticalPixelCoordinates.push_back(it->second.second);
		}
		m_PointToTrack.first = indexToSearch;
		// Takes the average
		m_PointToTrack.second = 0;
		for(auto it = vVerticalPixelCoordinates.begin(); it != vVerticalPixelCoordinates.end(); ++it)
			m_PointToTrack.second += *it;
		if(vVerticalPixelCoordinates.size()==0)
			m_PointToTrack.second = DBL_MAX;
		else
			m_PointToTrack.second /= vVerticalPixelCoordinates.size();

		ROS_INFO("Point to track %f %f", m_PointToTrack.first, m_PointToTrack.second);
	}

	// Predicts the next position of the point tracked and send a command so that the tool follows the point.
	// No filtering
	void UpdateArmCommand()
	{
		// Calculate the movement required for the arm to follow the interface
		double delta_tool_y = 0.0;

		double delta_tool_x;
		if(m_PointToTrack.second == DBL_MAX)
			delta_tool_x = 0.0;
		else
		{
			double K = 3.0;
			double temp = K*(m_PointToTrack.second - sParameters.floor_size_pix/2.0)/sParameters.floor_size_pix;
			delta_tool_x = -temp;
		}

		// For obstacles
		double delta_tool_z = 0;
		// Send the arm command
		geometry_msgs::Twist armCommand;
		armCommand.linear.x = delta_tool_x;
		ROS_INFO("armCommand.linear.x %f armCommand.linear.y %f",armCommand.linear.x, armCommand.linear.y);

		m_ArmTwistPublisher.publish(armCommand);
	}

	void FloorProjectorCallback(const sensor_msgs::Image msg)
	{
		try
		{
			m_pProcessedImage = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::MONO8);
		}
		catch (cv_bridge::Exception& e)
		{
			ROS_ERROR("cv_bridge exception: %s", e.what());
			return;
		}
		DetectInterface(m_Interface);
		// Actually, we may not use this
		ComputePointToTrack();
		UpdateArmCommand();
	}

	void MetalDetectorCallback(const std_msgs::Float32 v)
	{
		static int sMarkerID=0;

		if(v.data>0.95)	// Mine detected
		{
			//ROS_INFO("Found a mine");
			// Check whether a mine at the same position has already been found
			for(auto it = m_vMinesPositions.begin(); it!= m_vMinesPositions.end(); ++it)
			{
				if(squaredDistance2D(it->x(), it->y(), m_WorldSpaceToolPosition.x(), m_WorldSpaceToolPosition.y())<0.04)
					return;
			}
			//ROS_INFO("Publishing marker");

			visualization_msgs::Marker m;
			m.header.stamp = m_MsgHeaderStamp;
			m.header.frame_id = "/world";
			m.ns = "mine";
			m.id = sMarkerID++;
			m_vMinesPositions.push_back(m_WorldSpaceToolPosition);
			m.type = visualization_msgs::Marker::CYLINDER;
			m.action = visualization_msgs::Marker::ADD;
			m.pose.position.x = m_WorldSpaceToolPosition.x();
			m.pose.position.y = m_WorldSpaceToolPosition.y();
			m.pose.position.z = m_WorldSpaceToolPosition.z();
			m.scale.x = 0.4;
			m.scale.y = 0.4;
			m.scale.z = 0.4;
			m.color.a = 0.5;
			m.color.r = 0.0;
			m.color.g = 1.0;
			m.color.b = 0.0;
			//m.frame_locked = true;
			// Finally publish the marker
			m_MineMarkerPublisher.publish(m);
		}
	}

public:
	MineDetector() :
		m_NodeHandle("~")
	{
		// Make sure TF is ready
		ros::Duration(0.5).sleep();

		m_NodeHandle.param("floor_size_pix", sParameters.floor_size_pix, 500.0);

		m_FloorProjectorSubscriber = m_NodeHandle.subscribe("/floor_projector/edge", 1, &MineDetector::FloorProjectorCallback, this);
		m_RobotCommandSubscriber = m_NodeHandle.subscribe<geometry_msgs::Twist>("/vsv_driver/twistCommand",1,&MineDetector::RobotCommandCallback, this);
		m_MetalDetectorSubscriber = m_NodeHandle.subscribe("/vrep/metalDetector", 1, &MineDetector::MetalDetectorCallback, this);
		m_DepthSensorSubscriber = m_NodeHandle.subscribe("/vrep/depthSensor", 1, &MineDetector::DepthSensorCallback, this);
		m_MineMarkerPublisher = m_NodeHandle.advertise<visualization_msgs::Marker>("mine",1);
		m_ArmTwistPublisher = m_NodeHandle.advertise<geometry_msgs::Twist>("/arm_ik/twist",1);
	}
};

///////////////////////
// M   M  AA  I N  N //
// MM MM A  A I NN N //
// M M M AAAA I N NN //
// M   M A  A I N  N //
///////////////////////

int main(int argc, char * argv[]) {
	ros::init(argc, argv, "mine_detector");
	MineDetector node;
	ros::spin();
	return 0;
}
