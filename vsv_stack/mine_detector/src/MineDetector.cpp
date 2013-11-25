﻿#include <ros/ros.h>
#include <visualization_msgs/Marker.h>
#include <geometry_msgs/Twist.h>
#include <sensor_msgs/PointCloud2.h>
#include <sensor_msgs/Image.h>
#include <cv_bridge/cv_bridge.h>
#include <std_msgs/Float32.h>
#include <pcl_ros/point_cloud.h>
#include <pcl_ros/transforms.h>
#include <pcl/point_types.h>
#include <tf/tf.h>
#include <tf/transform_listener.h>

#include <sys/time.h>
#include <map>
#include <vector>

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
	ros::Subscriber m_FloorProjectorSubscriber;
	ros::Subscriber m_DepthSensorSubscriber;
	ros::Subscriber m_MetalDetectorSubscriber;
	ros::Publisher m_MineMarkerPublisher;
	ros::Publisher m_ArmTwistPublisher;
	tf::TransformListener m_Listener;

	ros::Time m_MsgHeaderStamp;
	std::vector<tf::Vector3> m_vMinesPositions;

	// State
	tf::Vector3 m_WorldSpaceRobotPosition;
	tf::Vector3 m_WorldSpaceToolPosition;
	double m_Orientation;

	geometry_msgs::Twist m_RobotSpeed;
	geometry_msgs::Twist m_ArmTipCommand;

	cv_bridge::CvImagePtr m_pProcessedImage;
	// Vector in which the indices correspond to the indices in m_pProcessedImage where the interface was found
	std::vector<std::pair<int,int>> m_Interface;

protected:
	//===============//
	// ROS Callbacks //

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
		m_Listener.waitForTransform("/world", "/body",
			m_MsgHeaderStamp, ros::Duration(1.0));
		tf::StampedTransform transform;
		m_Listener.lookupTransform("/world", "/body",
			m_MsgHeaderStamp, transform);

		m_WorldSpaceRobotPosition = transform * tf::Vector3(0.0, 0.0, 0.0);
		
		// Get the orientation of the robot
		auto Q = transform.getRotation();
		tf::Matrix3x3 M;
		M.setRotation(Q);
		double roll, pitch;
		M.getRPY(roll, pitch, m_Orientation);
	}

	// Detects the change of color in a picture (with a low number of different colors)
	// The result is store in a vector in which the indices correspond to the indices in the picture
	void DetectInterface(std::vector<std::pair<int,int>> &out)
	{
		unsigned char b, g, r, b_, g_, r_;
		out.clear();
		for( int row = 0; row < m_pProcessedImage->height; ++row )
		{
			for ( int col = 0; col < m_pProcessedImage->width; ++col )
			{
				//for( int z = 0; z < m_pProcessedImage->nChannels; ++z )
				//{
				//   c = m_pProcessedImage->imageData[m_pProcessedImage->widthStep * row + col * m_pProcessedImage->nChannels + z];
				//}
				b = m_pProcessedImage->imageData[img->widthStep * row + col * 3]
				g = m_pProcessedImage->imageData[img->widthStep * row + col * 3 + 1];
				r = m_pProcessedImage->imageData[img->widthStep * row + col * 3 + 2];
				for(int i=-1;i<=1; i=i+2)
				{
					for(int j=-1;j<=1; j=j+2)
					{
						int row_ = Clip(row+i,0,m_pProcessedImage->height);
						int col_ = Clip(col+j,0,m_pProcessedImage->width);
						b_ = m_pProcessedImage->imageData[img->widthStep * row_ + col_ * 3]
						g_ = m_pProcessedImage->imageData[img->widthStep * row_ + col_ * 3 + 1];
						r_ = m_pProcessedImage->imageData[img->widthStep * row_ + col_ * 3 + 2];
						if(b != b_ ||g != g_ ||r !== r_)
							out.push_back(std::make_pair(row_, col_));
					}
				}
			}
		}
	}

	// Predict the next position of the tool
	// No filtering
	void PredictToolPosition()
	{
		static Timer sTimer;
		static double sDeltaT = -INT_MAX;

		if(sDeltaT == -INT_MAX)
		{
			sTimer.start();
			sDeltaT = 0.0;
		}
		else
		{
			sDeltaT = sTimer.stop();
			sTimer.start();
		}

		//ROS_INFO("Time elapsed %f", sDeltaT);

		// Compute (x,y) the next position of the robot
		double theta = m_RobotSpeed.angular.z;
		double deltaTheta = theta*sDeltaT;

		double v = m_RobotSpeed.linear.x;
		double deltaX = v*cos(deltaTheta)*sDeltaT;
		double deltaY = v*sin(deltaTheta)*sDeltaT;

		double x = m_WorldSpaceRobotPosition.x() + deltaX;
		double y = m_WorldSpaceRobotPosition.y() + deltaY;

		
		double tool_x = m_WorldSpaceToolPosition.x() + m_ArmTipCommand.linear.x*sDeltaT;
		double tool_y = m_WorldSpaceToolPosition.y() + m_ArmTipCommand.linear.y*sDeltaT;
		double tool_z = m_WorldSpaceToolPosition.z() + m_ArmTipCommand.linear.z*sDeltaT;

		// We know that any mine would have been installed at the interface between the flat gray squares and the 
		// brown earth, so we need to make sure that the sensor stays close to it while we're driving.
		// Computes where the next point of the interface is after the robot moves to the predicted position (x, y), as follows :
		// 1) Applies a translation (deltaX, deltaY) to the line x=a, followed by a rotation of deltaTheta.
		// 2) Determines the intersection of the new line with the interface.
		// See the word report for pictures
		// The new origin is (deltaX, deltaY)
	
		// Normal vector (nx, ny) after rotation by deltaTheta
		nX=cos(deltaTheta);
		ny=sin(deltaTheta);
		// The equation of the new line after the transformation is :
		//nX(y-deltaY)-nY(x-deltaX)=0
		// Now determine the intersection between this line and the interface.
		// TODO: convert picture coordinates to coordinates in the projected image
		unsigned char b, g, r;
		for( int row = 0; row < m_pProcessedImage->height; ++row )
		{
			for ( int col = 0; col < m_pProcessedImage->width; ++col )
			{
				//for( int z = 0; z < m_pProcessedImage->nChannels; z++ )
				//{
				//   c = m_pProcessedImage->imageData[m_pProcessedImage->widthStep * row + col * m_pProcessedImage->nChannels + z];
				//}
				b = m_pProcessedImage->imageData[img->widthStep * row + col * 3]
				g = m_pProcessedImage->imageData[img->widthStep * row + col * 3 + 1];
				r = m_pProcessedImage->imageData[img->widthStep * row + col * 3 + 2];
			}
		}


		// TODO
		// Calculate the movement required for the arm to follow the interface
		double delta_tool_x = 0;
		double delta_tool_y = 0;
		// For obstacles
		double delta_tool_z = 0;
		// Send the arm command
		geometry_msgs::Twist armCommand;
		armCommand.linear.x = delta_tool_x/sDeltaT;
		armCommand.linear.y = delta_tool_y/sDeltaT;
		m_ArmTwistPublisher.publish(armCommand);
	}

	void FloorProjectorCallback(const sensor_msgs::Image msg)
	{
		try
		{
			m_pProcessedImage = cv_bridge::toCvCopy(msg, sensor_msgs::image_encodings::BGR8);
		}
		catch (cv_bridge::Exception& e)
		{
			ROS_ERROR("cv_bridge exception: %s", e.what());
			return;
		}
		DetectInterface(m_Interface);
	}

	void MetalDetectorCallback(const std_msgs::Float32 v)
	{
		static int sMarkerID=0;

		if(v.data>0.95)	// Mine detected
		{
			//ROS_INFO("Found a mine");
			//MineData.position = m_WorldSpaceRobotPosition;
			//MineData.probabilityOfdetection = v.data;
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

		m_FloorProjectorSubscriber = m_NodeHandle.subscribe("/floor_projector/floor", 1, &MineDetector::FloorProjectorCallback, this);
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
