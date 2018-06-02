#include <controller_interface/controller.h>
#include <hardware_interface/joint_command_interface.h>
#include <control_toolbox/pid.h>
#include <realtime_tools/realtime_buffer.h>

#include <pluginlib/class_list_macros.h>
#include <std_msgs/Float64MultiArray.h>
#include <angles/angles.h>

#include <urdf/model.h>

#include <kdl/tree.hpp>
#include <kdl/kdl.hpp>
#include <kdl/chain.hpp>
#include <kdl/chaindynparam.hpp>
#include <kdl_parser/kdl_parser.hpp>

#include <boost/scoped_ptr.hpp>

#include "arm_controllers/ControllerJointState.h"
#include "arm_controllers/PassivityControllerParamsConfig.h"

#define PI 3.141592
#define D2R PI/180.0
#define R2D 180.0/PI

namespace arm_controllers{

	class PassivityController: public controller_interface::Controller<hardware_interface::EffortJointInterface>
	{
		class Gains
		{
		public:
			Gains()
				//: /*alpha_(0.0), p_(0.0), i_(0.0, d_(0.0) */
			{}

			void setGains(/*double alpha,*/ double p, double i, double d)
			{
				/*alpha_ = alpha;*/ p_ = p; i_ = i; d_ = d;
			}

			void setGains()
			{
				
			}

			void getGains(/*double& alpha,*/ double& p, double& i, double& d)
			{
				/*alpha = alpha_; */p = p_; i = i_; d = d_;
			}

			void initDynamicReconfig(const ros::NodeHandle &node)
			{
				ROS_INFO("Init dynamic reconfig in namespace %s", node.getNamespace().c_str());

				// Start dynamic reconfigure server
				param_reconfig_server_.reset(new DynamicReconfigServer(param_reconfig_mutex_, node));
				dynamic_reconfig_initialized_ = true;

				// Set Dynamic Reconfigure's gains to Pid's values
				updateDynamicReconfig();

				// Set callback
				param_reconfig_callback_ = boost::bind(&Gains::dynamicReconfigCallback, this, _1, _2);
				param_reconfig_server_->setCallback(param_reconfig_callback_);
			}

			void updateDynamicReconfig()
			{
				// Make sure dynamic reconfigure is initialized
				if(!dynamic_reconfig_initialized_)
					return;

				// Get starting values
				arm_controllers::PassivityControllerParamsConfig config;

				// Get starting values
				getGains(/*config.alpha,*/ config.p, config.i, config.d);

				updateDynamicReconfig(config);
			}

			void updateDynamicReconfig(PassivityControllerParamsConfig config)
			{
				// Make sure dynamic reconfigure is initialized
				if(!dynamic_reconfig_initialized_)
					return;

				// Set starting values, using a shared mutex with dynamic reconfig
				param_reconfig_mutex_.lock();
				param_reconfig_server_->updateConfig(config);
				param_reconfig_mutex_.unlock();
			}

			void dynamicReconfigCallback(arm_controllers::PassivityControllerParamsConfig &config, uint32_t /*level*/)
			{
				ROS_DEBUG_STREAM_NAMED("passivity gain","Dynamics reconfigure callback recieved.");

				// Set the gains
				setGains(/*config.alpha,*/ config.p, config.i, config.d);
			}

			// double alpha_;
			double p_;
			double i_;
			double d_;

		private:
			// Store the gains in a realtime buffer to allow dynamic reconfigure to update it without
			// blocking the realtime update loop
			// realtime_tools::RealtimeBuffer<Gains> gains_buffer_;	// to do: consider real-time thread

			// Dynamics reconfigure
			bool dynamic_reconfig_initialized_;
			typedef dynamic_reconfigure::Server<arm_controllers::PassivityControllerParamsConfig> DynamicReconfigServer;
			boost::shared_ptr<DynamicReconfigServer> param_reconfig_server_;
			DynamicReconfigServer::CallbackType param_reconfig_callback_;

			boost::recursive_mutex param_reconfig_mutex_;
		};

		public:
		~PassivityController() 
		{
			command_sub_.shutdown();
			for (int i=0; i<n_joints_; i++)
			{
				if (gains_[i] != NULL)
				{
					delete gains_[i];
					gains_[i] = NULL;
				}
			}
		}

		bool init(hardware_interface::EffortJointInterface* hw, ros::NodeHandle &n)
  		{	
			loop_count_ = 0;
			// List of controlled joints
    		if (!n.getParam("joints", joint_names_))
			{
				ROS_ERROR("Could not find joint name");
				return false;
    		}
			n_joints_ = joint_names_.size();

			if(n_joints_ == 0)
			{
				ROS_ERROR("List of joint names is empty.");
				return false;
			}

			// urdf
			urdf::Model urdf;
			if (!urdf.initParam("robot_description"))
			{
				ROS_ERROR("Failed to parse urdf file");
            	return false;
			}

			// joint handle
			for(int i=0; i<n_joints_; i++)
			{
				try
				{
					joints_.push_back(hw->getHandle(joint_names_[i]));
				}
				catch (const hardware_interface::HardwareInterfaceException& e)
				{
					ROS_ERROR_STREAM("Exception thrown: " << e.what());
					return false;
				}

				urdf::JointConstSharedPtr joint_urdf = urdf.getJoint(joint_names_[i]);
				if (!joint_urdf)
				{
					ROS_ERROR("Could not find joint '%s' in urdf", joint_names_[i].c_str());
                	return false;
				}
				joint_urdfs_.push_back(joint_urdf); 
			}

			// kdl parser
			if (!kdl_parser::treeFromUrdfModel(urdf, kdl_tree_)){
				ROS_ERROR("Failed to construct kdl tree");
				return false;
			}

			// kdl chain
			std::string root_name, tip_name;
			if (!n.getParam("root_link", root_name))
			{
				ROS_ERROR("Could not find root link name");
				return false;
    		}
			if (!n.getParam("tip_link", tip_name))
			{
				ROS_ERROR("Could not find tip link name");
				return false;
    		}
			if(!kdl_tree_.getChain(root_name, tip_name, kdl_chain_))
			{
				ROS_ERROR_STREAM("Failed to get KDL chain from tree: ");
				ROS_ERROR_STREAM("  "<<root_name<<" --> "<<tip_name);
				ROS_ERROR_STREAM("  Tree has "<<kdl_tree_.getNrOfJoints()<<" joints");
				ROS_ERROR_STREAM("  Tree has "<<kdl_tree_.getNrOfSegments()<<" segments");
				ROS_ERROR_STREAM("  The segments are:");

				KDL::SegmentMap segment_map = kdl_tree_.getSegments();
            	KDL::SegmentMap::iterator it;

            	for( it=segment_map.begin(); it != segment_map.end(); it++ )
              		ROS_ERROR_STREAM( "    "<<(*it).first);

            	return false;
			}

			gravity_ = KDL::Vector::Zero();
			gravity_(2) = -9.81;
			M_.resize(n_joints_);
			C_.resize(n_joints_);
			G_.resize(n_joints_);
			
			// inverse dynamics solver
			id_solver_.reset( new KDL::ChainDynParam(kdl_chain_, gravity_) );

			// command and state
			tau_cmd_.data = Eigen::VectorXd::Zero(n_joints_);
			q_cmd_.data = Eigen::VectorXd::Zero(n_joints_);
			qdot_cmd_.data = Eigen::VectorXd::Zero(n_joints_);
			qddot_cmd_.data = Eigen::VectorXd::Zero(n_joints_);
			q_cmd_old_.data = Eigen::VectorXd::Zero(n_joints_);
			qdot_cmd_old_.data = Eigen::VectorXd::Zero(n_joints_);
			qdot_ref_.data = Eigen::VectorXd::Zero(n_joints_);
			qddot_ref_.data = Eigen::VectorXd::Zero(n_joints_);
			
			q_.data = Eigen::VectorXd::Zero(n_joints_);
			qdot_.data = Eigen::VectorXd::Zero(n_joints_);

			q_error_.data = Eigen::VectorXd::Zero(n_joints_);
			q_error_dot_.data = Eigen::VectorXd::Zero(n_joints_);
			q_error_int_.data = Eigen::VectorXd::Zero(n_joints_);


			// gains
			if (!n.getParam("gains/alpha", alpha_))
			{
				ROS_ERROR("Could not find alpha gain in %s", "gains/alpha");
				return false;
			}

			for (size_t i=0; i<n_joints_; i++)
			{
				gains_.push_back(new Gains());


				if (!n.getParam("gains/" + joint_names_[i] + "/pid/p", gains_[i]->p_))
				{
					ROS_ERROR("Could not find p gain in %s", ("gains/"+joint_names_[i]+"/pid/p").c_str());
					return false;
				}

				if (!n.getParam("gains/" + joint_names_[i] + "/pid/i", gains_[i]->i_))
				{
					ROS_ERROR("Could not find i gain in %s", ("gains/"+joint_names_[i]+"/pid/i").c_str());
					return false;
				}

				if (!n.getParam("gains/" + joint_names_[i] + "/pid/d", gains_[i]->d_))
				{
					ROS_ERROR("Could not find d gain in %s", ("gains/"+joint_names_[i]+"/pid/d").c_str());
					return false;
				}
				
				gains_[i]->initDynamicReconfig(ros::NodeHandle(n, "gains/"+joint_names_[i]));
			}

			// command subscriber
			commands_buffer_.writeFromNonRT(std::vector<double>(n_joints_, 0.0));
			command_sub_ = n.subscribe<std_msgs::Float64MultiArray>("command", 1, &PassivityController::commandCB, this);

			// start realtime state publisher
			controller_state_pub_.reset(
				new realtime_tools::RealtimePublisher<arm_controllers::ControllerJointState>(n, "state", 1));

			controller_state_pub_->msg_.header.stamp = ros::Time::now();
			for (size_t i=0; i<n_joints_; i++)
			{
				controller_state_pub_->msg_.name.push_back(joint_names_[i]);
				controller_state_pub_->msg_.command.push_back(0.0);
				controller_state_pub_->msg_.command_dot.push_back(0.0);
				controller_state_pub_->msg_.state.push_back(0.0);
				controller_state_pub_->msg_.state_dot.push_back(0.0);
				controller_state_pub_->msg_.error.push_back(0.0);
				controller_state_pub_->msg_.error_dot.push_back(0.0);
				controller_state_pub_->msg_.effort_command.push_back(0.0);
				controller_state_pub_->msg_.effort_feedforward.push_back(0.0);
				controller_state_pub_->msg_.effort_feedback.push_back(0.0);
			}

   			return true;
  		}

		void starting(const ros::Time& time)
		{
			// get joint positions
			for(size_t i=0; i<n_joints_; i++) 
			{
				q_(i) = joints_[i].getPosition();
				qdot_(i) = joints_[i].getVelocity();
				q_error_int_(i) = 0;
			}

			ROS_INFO("Starting Passivity Based Controller");
		}

		void commandCB(const std_msgs::Float64MultiArrayConstPtr& msg)
		{
			if(msg->data.size()!=n_joints_)
			{ 
			ROS_ERROR_STREAM("Dimension of command (" << msg->data.size() << ") does not match number of joints (" << n_joints_ << ")! Not executing!");
			return; 
			}
			commands_buffer_.writeFromNonRT(msg->data);
		}

  		void update(const ros::Time& time, const ros::Duration& period)
  		{
			std::vector<double> & commands = *commands_buffer_.readFromRT();
			double dt = period.toSec();
			double q_cmd_old;

			// get joint states
			// static double t = 0;
			for (size_t i=0; i<n_joints_; i++)
			{
				//q_cmd_(i) = M_PI/6*sin(t/10);//commands[i];
				q_cmd_(i) = commands[i];

				enforceJointLimits(q_cmd_(i), i);
				q_(i) = joints_[i].getPosition();
				qdot_(i) = joints_[i].getVelocity();

		        // Compute position error
				if (joint_urdfs_[i]->type == urdf::Joint::REVOLUTE)
				{
					angles::shortest_angular_distance_with_limits(
						q_(i),
						q_cmd_(i),
						joint_urdfs_[i]->limits->lower,
						joint_urdfs_[i]->limits->upper,
						q_error_(i));
				}
				else if (joint_urdfs_[i]->type == urdf::Joint::CONTINUOUS)
				{
					q_error_(i) = angles::shortest_angular_distance(q_(i), q_cmd_(i));
				}
				else //prismatic
				{
					q_error_(i) = q_cmd_(i) - q_(i);
				}
				q_error_dot_(i) = qdot_cmd_(i) - qdot_(i);
			}
			// t += dt;

			qdot_cmd_.data = (q_cmd_.data - q_cmd_old_.data) / period.toSec();;
			qddot_cmd_.data = (qdot_cmd_.data - qdot_cmd_old_.data) / period.toSec();
			
			q_error_dot_.data = qdot_cmd_.data - qdot_.data;

			q_cmd_old_ = q_cmd_;
			qdot_cmd_old_ = qdot_cmd_;

			// double dt = period.toSec();
			// if (t==10)
			// {
			// 	ROS_INFO("q_cmd = %.3f, %.3f, %.3f, %.3f, %.3f, %.3f", 
			// 		q_cmd_(0), q_cmd_(1), q_cmd_(2), q_cmd_(3), q_cmd_(4), q_cmd_(5));
			// 	t = 0;
			// }
			// t++;
			
			// compute dynamics term
			id_solver_->JntToMass(q_, M_);
			id_solver_->JntToCoriolis(q_, qdot_, C_);
			id_solver_->JntToGravity(q_, G_);

			// torque command
			qdot_ref_.data = qdot_cmd_.data + alpha_*q_error_.data;
			qddot_ref_.data = qddot_cmd_.data + alpha_*q_error_dot_.data;

			tau_cmd_.data = M_.data * qddot_ref_.data + C_.data + G_.data;

			for(int i=0; i<n_joints_; i++)
			{
				q_error_int_(i) += q_error_(i)*dt;

				controller_state_pub_->msg_.effort_feedforward[i] = tau_cmd_(i);
				tau_cmd_(i) += gains_[i]->p_*q_error_(i) + gains_[i]->i_*q_error_int_(i) + gains_[i]->d_*q_error_dot_(i);

				// effort saturation
				if (tau_cmd_(i) >= joint_urdfs_[i]->limits->effort)
					tau_cmd_(i) = joint_urdfs_[i]->limits->effort;
				
				if (tau_cmd_(i) <= -joint_urdfs_[i]->limits->effort)
					tau_cmd_(i) = -joint_urdfs_[i]->limits->effort;

				joints_[i].setCommand( tau_cmd_(i) );
			}

			// publish
			if (loop_count_ % 10 == 0)
			{
				if (controller_state_pub_->trylock())
				{
					controller_state_pub_->msg_.header.stamp = time;
					for(int i=0; i<n_joints_; i++)
					{
						controller_state_pub_->msg_.command[i] = R2D*q_cmd_(i);
						controller_state_pub_->msg_.command_dot[i] = R2D*qdot_cmd_(i);
						controller_state_pub_->msg_.state[i] = R2D*q_(i);
						controller_state_pub_->msg_.state_dot[i] = R2D*qdot_(i);
						controller_state_pub_->msg_.error[i] = R2D*q_error_(i);
						controller_state_pub_->msg_.error_dot[i] = R2D*q_error_dot_(i);
						controller_state_pub_->msg_.effort_command[i] = tau_cmd_(i);
						controller_state_pub_->msg_.effort_feedback[i] = tau_cmd_(i) - controller_state_pub_->msg_.effort_feedforward[i];
					}
					controller_state_pub_->unlockAndPublish();
				}
			}
  		}

  		void stopping(const ros::Time& time) { }

		void enforceJointLimits(double &command, unsigned int index)
		{
			// Check that this joint has applicable limits
			if (joint_urdfs_[index]->type == urdf::Joint::REVOLUTE || joint_urdfs_[index]->type == urdf::Joint::PRISMATIC)
			{
			if( command > joint_urdfs_[index]->limits->upper ) // above upper limnit
			{
				command = joint_urdfs_[index]->limits->upper;
			}
			else if( command < joint_urdfs_[index]->limits->lower ) // below lower limit
			{
				command = joint_urdfs_[index]->limits->lower;
			}
			}
		}
	private:
		int loop_count_;

		// joint handles
		unsigned int n_joints_;
		std::vector<std::string> joint_names_;
  		std::vector<hardware_interface::JointHandle> joints_;
		std::vector<urdf::JointConstSharedPtr> joint_urdfs_;

		// kdl
		KDL::Tree 	kdl_tree_;
		KDL::Chain	kdl_chain_;
		boost::scoped_ptr<KDL::ChainDynParam> id_solver_;	// inverse dynamics solver
		KDL::JntSpaceInertiaMatrix M_;
		KDL::JntArray C_;
		KDL::JntArray G_;									// gravity torque vector
		KDL::Vector gravity_;

		

		// cmd, state
		realtime_tools::RealtimeBuffer<std::vector<double> > commands_buffer_;
		KDL::JntArray tau_cmd_;
		KDL::JntArray q_cmd_, qdot_cmd_, qddot_cmd_, q_cmd_old_, qdot_cmd_old_;
		KDL::JntArray qdot_ref_, qddot_ref_;				// passivity-based control joint reference
		KDL::JntArray q_, qdot_;
		KDL::JntArray q_error_, q_error_dot_, q_error_int_;

		// gain
		double alpha_;
		std::vector<Gains*> gains_;

		// topic
		ros::Subscriber command_sub_;
		boost::scoped_ptr<
			realtime_tools::RealtimePublisher<
				arm_controllers::ControllerJointState> > controller_state_pub_;
	};

}

PLUGINLIB_EXPORT_CLASS(arm_controllers::PassivityController, controller_interface::ControllerBase)
