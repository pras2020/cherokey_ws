// This i s a ROS version of the standard "hel lo , world"
// program.

// This header def ines the standard ROS classes .
#include <ros/ros.h>

int main(int argc, char **argv)
{
    // Initialize the ROS system.
    ros::init(argc, argv, "cherokey_ros");

    // Establish this program as a ROS node .
    ros::NodeHandle nh;

    // Send some output as a log message .
    ROS_INFO_STREAM("Hello, ROS!");
}
