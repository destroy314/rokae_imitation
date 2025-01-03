#include <iostream>
#include <cmath>
#include <thread>
#include <atomic>
#include <mutex>
#include <array>
#include <chrono>
#include <string>
#include <zmq.hpp>
#include "json.hpp"
#include "rokae/robot.h"
#include "rokae/utility.h"

using json = nlohmann::json;


enum class CmdType {
    xyzrpy_vel, // 接收笛卡尔速度，发送笛卡尔位置
    pose_mat,   // 接收tcp变换矩阵，发送关节角度（期望接收频率接近1000Hz，否则会运动不平滑）
    joint_pose, // 接收关节角度，发送关节角度（期望接收频率接近1000Hz，否则会运动不平滑）
};

int main() {
    const char* zmq_recv_addr = "tcp://localhost:5555";

    // 使用位置控制模式，否则为阻抗控制
    const bool usePositionControl = true;
    // 使用期望的当前位置（否则为实时查询到的，此时由于传给控制器的位置差总是很小动作会很慢，需要增大最大速度）（仅在xyzrpy_vel时有效）
    const bool useDesiredPose = true;
    // 解释命令为相对于工具坐标系的移动，否则相对于基座标系（仅在xyzrpy_vel时有效）
    const bool useTCPMove = false;

    const CmdType cmdType = CmdType:: pose_mat;

    // 假设期望速度在 [-1, 1] 范围内进行标准化
    const double max_linear_velocity = 0.08;  // 最大线速度 (米/秒)
    const double max_angular_velocity = 0.16;  // 最大角速度 (弧度/秒)
    // TODO 为什么调小它反而变快了？？？？

    std::cout.setf(std::ios::showpoint);
    std::cout.precision(4);

    std::error_code ec;

    try {
        std::string robot_ip = "192.168.0.160"; // 机器人的 IP 地址
        std::string local_ip = "192.168.0.100"; // 本地机器的 IP 地址

        rokae::xMateErProRobot robot(robot_ip, local_ip);

        // 设置网络容差、操作模式、运动控制模式和电源状态
        robot.setRtNetworkTolerance(10, ec);
        robot.setOperateMode(rokae::OperateMode::automatic, ec);
        robot.setMotionControlMode(rokae::MotionControlMode::RtCommand, ec);
        robot.setPowerState(true, ec);

        // 实时运动控制器
        auto rtCon = robot.getRtMotionController().lock();

        // 工具中心点在法兰前方0.2m，坐标系相同
        std::array<double, 16> tcp_frame = {1, 0, 0, 0,
                                            0, 1, 0, 0,
                                            0, 0, 1, 0.2,
                                            0, 0, 0, 1};
        // std::array<double, 16> tcp_frame = {1, 0, 0, 0,
        //                                     0, 1, 0, 0,
        //                                     0, 0, 1, 0,
        //                                     0, 0, 0, 1};
        // 将被控制坐标系设置为工具坐标系，影响callback应该返回的变换矩阵定义，但不影响robot.posture()返回的姿态，无论ct为何
        rtCon->setEndEffectorFrame(tcp_frame, ec);

        rtCon->setFilterFrequency(25, 25, 52, ec);

        if (usePositionControl) {
            // 设置碰撞检测阈值
            rtCon->setCollisionBehaviour({16, 16, 8, 8, 4, 4, 4}, ec);
        } else {
            // 设置阻抗系数
            // TODO 似乎没用？
            // rtCon->setFcCoor(tcp_frame, rokae::FrameType::tool, ec);
            if (cmdType == CmdType::xyzrpy_vel || cmdType == CmdType::pose_mat) {
                rtCon->setCartesianImpedance({1200, 1200, 1200, 100, 100, 100}, ec);
                // TODO 似乎没用？
                // rtCon->setCartesianImpedanceDesiredTorque({0, 0, 0, 0, 0, 0}, ec);
            } else if (cmdType == CmdType::joint_pose) {
                rtCon->setJointImpedance({1200, 1200, 1200, 100, 100, 100, 100}, ec);
            }
        }

        // 移动到初始位置
        std::array<double, 7> initial_joint_positions = {0, M_PI / 6, 0, M_PI / 3, 0, M_PI / 2, 0};
        rtCon->MoveJ(0.3, robot.jointPos(ec), initial_joint_positions);

        if(!ec){
            std::cout << "初始化成功" << std::endl;
        }

        if (usePositionControl) {
            if (cmdType == CmdType::xyzrpy_vel || cmdType == CmdType::pose_mat) {
                rtCon->startMove(rokae::RtControllerMode::cartesianPosition);
            } else if (cmdType == CmdType::joint_pose) {
                rtCon->startMove(rokae::RtControllerMode::jointPosition);
            }
        } else {
            if (cmdType == CmdType::xyzrpy_vel || cmdType == CmdType::pose_mat) {
                rtCon->startMove(rokae::RtControllerMode::cartesianImpedance);
            } else if (cmdType == CmdType::joint_pose) {
                rtCon->startMove(rokae::RtControllerMode::jointImpedance);
            }
        }

        std::atomic<bool> running(true);

        // zmq 获取的速度命令
        std::mutex command_mutex;
        std::array<double, 3> linear_velocity_cmd = {0.0, 0.0, 0.0};    // [vx, vy, vz]
        std::array<double, 3> angular_velocity_cmd = {0.0, 0.0, 0.0};   // [wx, wy, wz]
        std::array<double, 16> pose_matrix_cmd = {0.0};
        std::vector<double> joint_position_cmd;
        bool command_supressed = false; // 用于暂停控制

        // 跟踪最后一次接收到 zmq 消息的时间
        std::chrono::steady_clock::time_point last_message_time = std::chrono::steady_clock::now();
        const std::chrono::milliseconds timeout_duration(100);

        // 共享变量用于当前姿态
        std::mutex pose_mutex;
        std::array<double, 6> current_posture = {0.0};
        std::array<double, 7> current_joint = {0.0};

        // ZeroMQ 订阅线程接收期望的速度
        auto zmq_receiver = [&]() {
            zmq::context_t context(1);
            zmq::socket_t subscriber(context, ZMQ_SUB);
            subscriber.connect(zmq_recv_addr);
            subscriber.setsockopt(ZMQ_SUBSCRIBE, "", 0); // 订阅所有消息
            std::chrono::steady_clock::time_point last_time = std::chrono::steady_clock::now();

            std::array<double, 3> linear_velocity;
            std::array<double, 3> angular_velocity;
            std::array<double, 16> pose_matrix;
            std::array<double, 7> joint_position;
            
            while (running) {
                zmq::message_t message;
                zmq::recv_result_t received = subscriber.recv(message, zmq::recv_flags::none); // 阻塞接收消息
                if (received) {
                    auto current_time = std::chrono::steady_clock::now();
                    std::chrono::duration<double, std::milli> elapsed = current_time - last_time;
                    double dt = elapsed.count();
                    last_time = current_time;

                    std::string msg_str(static_cast<char*>(message.data()), message.size());
                    json msg_json = json::parse(msg_str);
                    // std::cout << msg_json << "\n";
                    if (msg_json.contains("linear_velocity") && msg_json.contains("angular_velocity")) {
                        linear_velocity = msg_json["linear_velocity"].get<std::array<double, 3>>();
                        angular_velocity = msg_json["angular_velocity"].get<std::array<double, 3>>();
                        {
                            std::lock_guard<std::mutex> lock(command_mutex);
                            linear_velocity_cmd = linear_velocity;
                            angular_velocity_cmd = angular_velocity;
                            command_supressed = false;
                            last_message_time = current_time;
                        }
                        std::cout << "zmq recv v=[" << linear_velocity[0] << ", " << linear_velocity[1] << ", " << linear_velocity[2] << "] elapsed=" << dt << "ms" << std::endl;
                    } else if (msg_json.contains("pose_matrix")){
                        pose_matrix = msg_json["pose_matrix"].get<std::array<double, 16>>();
                        {
                            std::lock_guard<std::mutex> lock(command_mutex);
                            pose_matrix_cmd = pose_matrix;
                            command_supressed = false;
                            last_message_time = current_time;
                        }
                    } else if (msg_json.contains("joint_position")){
                        joint_position = msg_json["joint_position"].get<std::array<double, 7>>();
                        {
                            std::lock_guard<std::mutex> lock(command_mutex);
                            joint_position_cmd.clear();
                            std::copy(joint_position.begin(), joint_position.end(), std::back_inserter(joint_position_cmd));
                            command_supressed = true;
                            last_message_time = current_time;
                        }
                    } else {
                        std::cerr << "未知的zmq控制命令" << msg_json << std::endl;
                    }
                }
            }
        };

        // ZeroMQ 发布者线程发送当前末端位置
        auto zmq_sender = [&]() {
            zmq::context_t context(1);
            zmq::socket_t publisher(context, ZMQ_PUB);
            publisher.bind("tcp://localhost:5556"); // 根据需要调整地址和端口

            while (running) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100)); // 发送频率为10Hz

                if (cmdType == CmdType::xyzrpy_vel || cmdType == CmdType::pose_mat){
                    // 复制当前姿态
                    std::array<double, 6> posture_copy;
                    {
                        std::lock_guard<std::mutex> lock(pose_mutex);
                        posture_copy = current_posture;
                    }

                    // 创建 JSON 消息
                    json msg_json;
                    msg_json["ActualTCPPose"] = {posture_copy[0], posture_copy[1], posture_copy[2],posture_copy[3], posture_copy[4], posture_copy[5]};

                    std::string msg_str = msg_json.dump();
                    // std::cout<<msg_str<<"\n";
                    // 发送消息
                    zmq::message_t message(msg_str.size());
                    memcpy(message.data(), msg_str.c_str(), msg_str.size());
                    publisher.send(message, zmq::send_flags::none);
                } else if (cmdType == CmdType::joint_pose ){
                    // 复制当前关节角
                    std::array<double, 7> joint_copy;
                    {
                        std::lock_guard<std::mutex> lock(pose_mutex);
                        joint_copy = current_joint;
                    }

                    // 创建 JSON 消息
                    json msg_json;
                    msg_json["ActualJointPos"] = {joint_copy[0], joint_copy[1], joint_copy[2], joint_copy[3], joint_copy[4], joint_copy[5], joint_copy[6]};

                    std::string msg_str = msg_json.dump();
                    // std::cout<<msg_str<<"\n";
                    // 发送消息
                    zmq::message_t message(msg_str.size());
                    memcpy(message.data(), msg_str.c_str(), msg_str.size());
                    publisher.send(message, zmq::send_flags::none);
                }

            }
        };

        // 用于打印日志的变量
        std::array<double, 3> last_pos = {0.0, 0.0, 0.0};
        std::array<double, 3> curr_pos = {0.0, 0.0, 0.0};

        // callback 返回的目标变换矩阵/关节角度，使用当前值初始化
        std::array<double, 16> target_pose_matrix;
        current_posture = robot.posture(rokae::CoordinateType::flangeInBase, ec);
        rokae::Utils::postureToTransArray(current_posture, target_pose_matrix);
        pose_matrix_cmd = target_pose_matrix;

        std::vector<double> target_joint_pose;
        current_joint = robot.jointPos(ec);
        std::copy(current_joint.begin(), current_joint.end(), std::back_inserter(target_joint_pose));
        joint_position_cmd = target_joint_pose;

        std::thread zmq_sender_thread(zmq_sender);
        std::thread zmq_receiver_thread(zmq_receiver);

        // 使用 tcp_frame 计算当前tcp 在 base 中的坐标来初始化 target_pose_matrix
        std::array<double, 16> tcp_in_base;
        for (int i = 0; i < 4; ++i) {
            for (int j = 0; j < 4; ++j) {
            tcp_in_base[i * 4 + j] = 0;
            for (int k = 0; k < 4; ++k) {
                tcp_in_base[i * 4 + j] += target_pose_matrix[i * 4 + k] * tcp_frame[k * 4 + j];
            }
            }
        }
        target_pose_matrix = tcp_in_base;

        // 轴空间控制时的回调函数
        std::function<rokae::JointPosition(void)> callback_joint = [&, rtCon]() -> rokae::JointPosition {
            auto start1 = std::chrono::steady_clock::now();
            // 获取当前的机器人状态：末端执行器姿态/轴角
            // TODO 耗时可能比较长，切换为setControlLoop(useStateDataInLoop=True)和getStateData
            std::array<double, 6> current_posture_local = robot.posture(rokae::CoordinateType::flangeInBase, ec);
            std::array<double, 7> joint_pose_local = robot.jointPos(ec);
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double, std::milli> dur1 = now - start1;
            if (dur1.count() > 1){
                std::cout<< "robot.posture=" << dur1.count() << "ms" << std::endl;
            }
            // 更新共享的当前姿态
            {
                current_posture = current_posture_local;
                current_joint = joint_pose_local;
            }

            {
                target_joint_pose = joint_position_cmd;
            }
            std::cout << target_joint_pose[3];
            std::cout << std::endl;
            return rokae::JointPosition(target_joint_pose);
        };

        // 笛卡尔空间控制时的回调函数
        std::function<rokae::CartesianPosition(void)> callback_cart = [&, rtCon]() -> rokae::CartesianPosition {
            auto callback_start = std::chrono::steady_clock::now();

            // 计算时间步长，避免过大值
            // std::chrono::duration<double> elapsed = callback_start - last_time;
            // double dt = elapsed.count();
            // dt = std::min(dt, 0.002);
            // last_time = callback_start;

            double dt = 0.001;

            auto start1 = std::chrono::steady_clock::now();
            // 获取当前的机器人状态：末端执行器姿态/轴角
            // TODO 耗时可能比较长，切换为setControlLoop(useStateDataInLoop=True)和getStateData
            std::array<double, 6> current_posture_local = robot.posture(rokae::CoordinateType::flangeInBase, ec);
            std::array<double, 7> joint_pose_local = robot.jointPos(ec);
            auto now = std::chrono::steady_clock::now();
            std::chrono::duration<double, std::milli> dur1 = now - start1;
            if (dur1.count() > 1){
                std::cout<< "robot.posture=" << dur1.count() << "ms" << std::endl;
            }
            // 更新共享的当前姿态
            {
                std::lock_guard<std::mutex> lock(pose_mutex);
                current_posture = current_posture_local;
                current_joint = joint_pose_local;
            }

            // 接收tcp变换矩阵时直接返回
            if (cmdType == CmdType::pose_mat){
                {
                    std::lock_guard<std::mutex> lock(command_mutex);
                    target_pose_matrix = pose_matrix_cmd;
                }
                return rokae::CartesianPosition(target_pose_matrix);
            }

            // 使用实时查询到的位置作为运动起点
            if(!useDesiredPose){
                rokae::Utils::postureToTransArray(current_posture_local, target_pose_matrix);
            }

            curr_pos = {target_pose_matrix[3], target_pose_matrix[7], target_pose_matrix[11]};

            // 从姿态矩阵中提取当前的旋转矩阵
            std::array<double, 9> current_rotation_matrix = {
                target_pose_matrix[0], target_pose_matrix[1], target_pose_matrix[2],
                target_pose_matrix[4], target_pose_matrix[5], target_pose_matrix[6],
                target_pose_matrix[8], target_pose_matrix[9], target_pose_matrix[10]
            };

            std::array<double, 3> linear_velocity;
            std::array<double, 3> angular_velocity;
            {
                std::lock_guard<std::mutex> lock(command_mutex);
                auto time_since_last_msg = std::chrono::duration_cast<std::chrono::milliseconds>(callback_start - last_message_time);
                if (time_since_last_msg > timeout_duration && !command_supressed) {
                    // 超时，设置速度为0并输出错误
                    linear_velocity_cmd = {0.0, 0.0, 0.0};
                    angular_velocity_cmd = {0.0, 0.0, 0.0};
                    command_supressed = true;
                    std::cerr << "警告: 未在 " << timeout_duration.count() << " 毫秒内接收到 ZeroMQ 消息。将期望速度置为0。" << std::endl;
                }

                linear_velocity = linear_velocity_cmd;
                angular_velocity = angular_velocity_cmd;
            }

            // TCP（法兰）坐标系的前、左、上分别是z、y、-x
            if(useTCPMove){
                linear_velocity = {-linear_velocity[2], linear_velocity[1], linear_velocity[0]};
                angular_velocity = {-angular_velocity[2], angular_velocity[1], angular_velocity[0]};
            }

            // 计算缩放后的速度
            for (int i = 0; i < 3; ++i) {
                linear_velocity[i] *= max_linear_velocity;
                angular_velocity[i] *= max_angular_velocity;
            }

            // 计算位置变化 (delta_position = linear_velocity * dt)
            std::array<double, 3> delta_position;
            for (int i = 0; i < 3; ++i) {
                delta_position[i] = linear_velocity[i] * dt;
            }

            // 将delta_position从工具坐标系转换到基坐标系
            if (useTCPMove) {
                std::array<double, 3> transformed_delta_position = {0.0, 0.0, 0.0};
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        transformed_delta_position[i] += current_rotation_matrix[i * 3 + j] * delta_position[j];
                    }
                }
                delta_position = transformed_delta_position;
            }

            // 更新姿态矩阵中的位置
            target_pose_matrix[3] += delta_position[0];   // X
            target_pose_matrix[7] += delta_position[1];   // Y
            target_pose_matrix[11] += delta_position[2];  // Z

            // 计算方向变化
            // 将角速度转换为旋转向量
            std::array<double, 3> delta_rotation_vector;
            for (int i = 0; i < 3; ++i) {
                delta_rotation_vector[i] = angular_velocity[i] * dt;
            }

            // 将delta_rotation_vector从工具坐标系转换到基坐标系
            if (useTCPMove) {
                std::array<double, 3> transformed_delta_rotation_vector = {0.0, 0.0, 0.0};
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        transformed_delta_rotation_vector[i] += current_rotation_matrix[i * 3 + j] * delta_rotation_vector[j];
                    }
                }
                delta_rotation_vector = transformed_delta_rotation_vector;
            }

            // 计算旋转角度和轴
            double angle = std::sqrt(delta_rotation_vector[0] * delta_rotation_vector[0] +
                                     delta_rotation_vector[1] * delta_rotation_vector[1] +
                                     delta_rotation_vector[2] * delta_rotation_vector[2]);

            std::array<double, 9> delta_rotation_matrix = {1, 0, 0,
                                                           0, 1, 0,
                                                           0, 0, 1};

            if (angle > 1e-6) {
                // 归一化旋转向量以得到旋转轴
                std::array<double, 3> axis = {delta_rotation_vector[0] / angle,
                                               delta_rotation_vector[1] / angle,
                                               delta_rotation_vector[2] / angle};

                // 使用罗德里格公式计算旋转矩阵
                double c = std::cos(angle);
                double s = std::sin(angle);
                double t = 1 - c;
                double x = axis[0];
                double y = axis[1];
                double z = axis[2];

                delta_rotation_matrix = {
                    t * x * x + c,     t * x * y - s * z, t * x * z + s * y,
                    t * x * y + s * z, t * y * y + c,     t * y * z - s * x,
                    t * x * z - s * y, t * y * z + s * x, t * z * z + c
                };
            }

            // 计算新的旋转矩阵: new_R = delta_R * current_R
            std::array<double, 9> new_rotation_matrix = {0};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    for (int k = 0; k < 3; ++k) {
                        new_rotation_matrix[i * 3 + j] += delta_rotation_matrix[i * 3 + k] * current_rotation_matrix[k * 3 + j];
                    }
                }
            }

            // 更新姿态矩阵中的旋转矩阵
            target_pose_matrix[0] = new_rotation_matrix[0];
            target_pose_matrix[1] = new_rotation_matrix[1];
            target_pose_matrix[2] = new_rotation_matrix[2];
            target_pose_matrix[4] = new_rotation_matrix[3];
            target_pose_matrix[5] = new_rotation_matrix[4];
            target_pose_matrix[6] = new_rotation_matrix[5];
            target_pose_matrix[8] = new_rotation_matrix[6];
            target_pose_matrix[9] = new_rotation_matrix[7];
            target_pose_matrix[10] = new_rotation_matrix[8];

            // for (int i = 0; i < 4; ++i) {
            //     for (int j = 0; j < 4; ++j) {
            //         std::cout << target_pose_matrix[i * 4 + j] << " ";
            //     }
            //     std::cout << std::endl;
            // }

            // 测量回调执行时间，打印日志
            auto callback_end = std::chrono::steady_clock::now();
            std::chrono::duration<double, std::milli> callback_duration = callback_end - callback_start;
            // std::cout << "p=[" << curr_pos[0] << ", " << curr_pos[1] << ", " << curr_pos[2] << "] "
            // << "dp=[" << delta_position[0] << ", " << delta_position[1] << ", " << delta_position[2] << "] ";
            if(!useDesiredPose){
                std::cout << "rdp=[" << curr_pos[0]-last_pos[0] << ", " << curr_pos[1]-last_pos[1] << ", " << curr_pos[2]-last_pos[2] << "] ";
            }
            // std::cout << "dt=" << dt*1000 << "ms exe=" << callback_duration.count() << "ms" << std::endl;
            last_pos = curr_pos;

            return rokae::CartesianPosition(target_pose_matrix);
        };

        if (cmdType == CmdType::joint_pose) {
            rtCon->setControlLoop(callback_joint);
        } else {
            rtCon->setControlLoop(callback_cart);
        };
        rtCon->startLoop(false);

        std::cout << "开始实时控制，按回车键停止..." << std::endl;
        std::cin.get();

        rtCon->stopLoop();
        std::cout << "控制循环已停止" << std::endl;

        running = false;
        zmq_receiver_thread.join();
        zmq_sender_thread.join();

        robot.setPowerState(false, ec);
    } catch (const std::exception &e) {
        std::cerr << "捕获异常: " << e.what() << "ec=" << ec << std::endl;
    }

    return 0;
}
