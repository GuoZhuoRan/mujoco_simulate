#include <mujoco/mujoco.h>
#include <GLFW/glfw3.h>
#include <cstdio>
#include <iostream>
#include "useful_math.h"
#include "GLFW_callbacks.h"
#include "MJ_Interface.h"
#include "PVT_Ctr.h"
#include "Pin_KinDyn.h"
#include "DataLogger.h"
#include "WBC_priority.h"
#include "GaitScheduler.h"
#include "FootPlacement.h"
#include "JoyStickInterpreter.h"

// MuJoCo load and compile model
char error[1000] = "Could not load binary model";
mjModel* mj_model = mj_loadXML("../Models/scene_board.xml", 0, error, 1000);
mjData* mj_data = mj_makeData(mj_model);

//************************
// main function
int main(int argc, const char** argv)
{
    // ini classes
    UIctr uiController(mj_model,mj_data);   // UI control for Mujoco
    MJ_Interface mj_interface(mj_model, mj_data); // data interface for Mujoco
    Pin_KinDyn kinDynSolver("../Models/OGHR_wholeBody.urdf"); // kinematics and dynamics solver
    DataBus RobotState(kinDynSolver.model_nv); // data bus
    WBC_priority WBC_solv(kinDynSolver.model_nv, 18, 22, 0.7, mj_model->opt.timestep); // WBC solver
    GaitScheduler gaitScheduler(0.4, mj_model->opt.timestep); // gait scheduler
    PVT_Ctr pvtCtr(mj_model->opt.timestep,"../Common/JointCtrConfig.json");// PVT joint control
    FootPlacement footPlacement; // foot-placement planner
    JoyStickInterpreter jsInterp(mj_model->opt.timestep); // desired baselink velocity generator
    DataLogger logger("../RecordedData/DataLogger.log"); // data logger

    // variables ini
    double stand_legLength = 1.01; //-0.95; // desired baselink height
    double foot_height =0.07; // distance between the foot ankel joint and the bottom
    double  xv_des = 0.7;  // desired velocity in x direction

    RobotState.width_hips = 0.229;
    footPlacement.kp_vx = 0.03;
    footPlacement.kp_vy = 0.03;
    footPlacement.kp_wz = 0.03;
    footPlacement.stepHeight = 0.2;
    footPlacement.legLength=stand_legLength;
    //mju_copy(mj_data->qpos, mj_model->key_qpos, mj_model->nq*1); // set ini pos in Mujoco
    int model_nv=kinDynSolver.model_nv;

    // ini position and posture for foot-end and hand
    std::vector<double> motors_pos_des(model_nv-6,0);
    std::vector<double> motors_pos_cur(model_nv-6,0);
    std::vector<double> motors_vel_des(model_nv-6,0);
    std::vector<double> motors_vel_cur(model_nv-6,0);
    std::vector<double> motors_tau_des(model_nv-6,0);
    std::vector<double> motors_tau_cur(model_nv-6,0);
    Eigen::Vector3d fe_l_pos_L_des={-0.018, 0.113, -stand_legLength};
    Eigen::Vector3d fe_r_pos_L_des={-0.018, -0.116, -stand_legLength};
    Eigen::Vector3d fe_l_eul_L_des={-0.000, -0.008, -0.000};
    Eigen::Vector3d fe_r_eul_L_des={0.000, -0.008, 0.000};
    Eigen::Matrix3d fe_l_rot_des= eul2Rot(fe_l_eul_L_des(0),fe_l_eul_L_des(1),fe_l_eul_L_des(2));
    Eigen::Matrix3d fe_r_rot_des= eul2Rot(fe_r_eul_L_des(0),fe_r_eul_L_des(1),fe_r_eul_L_des(2));

    Eigen::Vector3d hd_l_pos_L_des={-0.019, 0.400, -0.159};
    Eigen::Vector3d hd_r_pos_L_des={-0.019, -0.404, -0.159};
    Eigen::Vector3d hd_l_eul_L_des={-1.280, -0.223, -0.186};
    Eigen::Vector3d hd_r_eul_L_des={1.280, -0.223, 0.186};
    Eigen::Matrix3d hd_l_rot_des= eul2Rot(hd_l_eul_L_des(0),hd_l_eul_L_des(1),hd_l_eul_L_des(2));
    Eigen::Matrix3d hd_r_rot_des= eul2Rot(hd_r_eul_L_des(0),hd_r_eul_L_des(1),hd_r_eul_L_des(2));

    auto resLeg=kinDynSolver.computeInK_Leg(fe_l_rot_des,fe_l_pos_L_des,fe_r_rot_des,fe_r_pos_L_des);
    auto resHand=kinDynSolver.computeInK_Hand(hd_l_rot_des,hd_l_pos_L_des,hd_r_rot_des,hd_r_pos_L_des);
    Eigen::VectorXd qIniDes=Eigen::VectorXd::Zero(mj_model->nq,1);
    qIniDes.block(7,0,mj_model->nq-7,1)=resLeg.jointPosRes+resHand.jointPosRes;
    WBC_solv.setQini(qIniDes);

    // register variable name for data logger
    logger.addIterm("simTime", 1);
    logger.addIterm("motors_pos_cur",model_nv-6);
    logger.addIterm("motors_pos_des",model_nv-6);
    logger.addIterm("motors_tau_cur",model_nv-6);
    logger.addIterm("motors_vel_des",model_nv-6);
    logger.addIterm("motors_vel_cur",model_nv-6);
    logger.finishItermAdding();

    /// ----------------- sim Loop ---------------
    double simEndTime=20;
    mjtNum simstart = mj_data->time;
    double simTime = mj_data->time;
    double startSteppingTime=3;
    double startWalkingTime=5;

    // init UI: GLFW
    uiController.iniGLFW();
    uiController.enableTracking(); // enable viewpoint tracking of the body 1 of the robot
    uiController.createWindow("Demo",false);

    while( !glfwWindowShouldClose(uiController.window))
    {
        // advance interactive simulation for 1/60 sec
        //  Assuming MuJoCo can simulate faster than real-time, which it usually can,
        //  this loop will finish on time for the next frame to be rendered at 60 fps.
        //  Otherwise add a cpu timer and exit this loop when it is time to render.
        simstart=mj_data->time;
        while( mj_data->time - simstart < 1.0/60.0 )
        {
            mj_step(mj_model, mj_data);

            simTime=mj_data->time;
            printf("-------------%.3f s------------\n",simTime);
            mj_interface.updateSensorValues();
            mj_interface.dataBusWrite(RobotState);

            // update kinematics and dynamics info
            kinDynSolver.dataBusRead(RobotState);
            kinDynSolver.computeJ_dJ();
            kinDynSolver.computeDyn();
            kinDynSolver.dataBusWrite(RobotState);

            // Enter here functions to send actuator commands, like:
            // arm-l: 0-6, arm-r: 7-13, head: 14, waist: 15-17, leg-l: 18-23, leg-r: 24-29

            if (simTime > startWalkingTime) {
                jsInterp.setWzDesLPara(0, 1);
                jsInterp.setVxDesLPara(xv_des, 2.0); // jsInterp.setVxDesLPara(0.9,1);
            } else
                jsInterp.setIniPos(RobotState.q(0), RobotState.q(1));
            jsInterp.step();
            RobotState.js_pos_des(2) = stand_legLength + foot_height; // pos z is not assigned in jyInterp
            jsInterp.dataBusWrite(RobotState); // only pos x, pos y, theta z, vel x, vel y , omega z are rewrote.

            if (simTime >= startSteppingTime) {
                // gait scheduler
                gaitScheduler.dataBusRead(RobotState);
                gaitScheduler.step();
                gaitScheduler.dataBusWrite(RobotState);

                footPlacement.dataBusRead(RobotState);
                footPlacement.getSwingPos();
                footPlacement.dataBusWrite(RobotState);
            }

            // ------------- WBC ------------
            // WBC input
            RobotState.Fr_ff = Eigen::VectorXd::Zero(12);
            RobotState.des_ddq = Eigen::VectorXd::Zero(mj_model->nv);
            RobotState.des_dq = Eigen::VectorXd::Zero(mj_model->nv);
            RobotState.des_delta_q = Eigen::VectorXd::Zero(mj_model->nv);
            RobotState.base_rpy_des << 0, 0, jsInterp.thetaZ;
            RobotState.base_pos_des(2) = stand_legLength+foot_height;

            RobotState.Fr_ff<<0,0,370,0,0,0,
                    0,0,370,0,0,0;

            // adjust des_delata_q, des_dq and des_ddq to achieve forward walking
            if (simTime > startWalkingTime + 1) {
                RobotState.des_delta_q.block<2, 1>(0, 0) << jsInterp.vx_W * mj_model->opt.timestep, jsInterp.vy_W * mj_model->opt.timestep;
                RobotState.des_delta_q(5) = jsInterp.wz_L * mj_model->opt.timestep;
                RobotState.des_dq.block<2, 1>(0, 0) << jsInterp.vx_W, jsInterp.vy_W;
                RobotState.des_dq(5) = jsInterp.wz_L;

                double k = 5;
                RobotState.des_ddq.block<2, 1>(0, 0) << k * (jsInterp.vx_W - RobotState.dq(0)), k * (jsInterp.vy_W -
                                                                                                     RobotState.dq(1));
                RobotState.des_ddq(5) = k * (jsInterp.wz_L - RobotState.dq(5));
            }


            // WBC Calculation
            WBC_solv.dataBusRead(RobotState);
            WBC_solv.computeDdq();
            WBC_solv.computeTau();
            WBC_solv.dataBusWrite(RobotState);

            // get the final joint command
            if (simTime<=startSteppingTime){
                RobotState.motors_pos_des= eigen2std(resLeg.jointPosRes+resHand.jointPosRes);
                RobotState.motors_vel_des=motors_vel_des;
                RobotState.motors_tor_des=motors_tau_des;
            }
            else
            {
                Eigen::VectorXd pos_des=kinDynSolver.integrateDIY(RobotState.q, RobotState.wbc_delta_q_final);
                RobotState.motors_pos_des = eigen2std(pos_des.block(7,0, model_nv-6,1));
                RobotState.motors_vel_des = eigen2std(RobotState.wbc_dq_final);
                RobotState.motors_tor_des = eigen2std(RobotState.wbc_tauJointRes);
            }

            pvtCtr.dataBusRead(RobotState);
            if (simTime<=3)
            {
                pvtCtr.calMotorsPVT(100.0/1000.0/180.0*3.1415);
            }
            else
            {
                pvtCtr.setJointPD(100,10,"Joint-ankel-l-pitch");
                pvtCtr.setJointPD(100,10,"Joint-ankel-l-roll");
                pvtCtr.setJointPD(100,10,"Joint-ankel-r-pitch");
                pvtCtr.setJointPD(100,10,"Joint-ankel-r-roll");
                pvtCtr.setJointPD(1000,100,"Joint-knee-l-pitch");
                pvtCtr.setJointPD(1000,100,"Joint-knee-r-pitch");
                pvtCtr.calMotorsPVT();
            }
            pvtCtr.dataBusWrite(RobotState);

            mj_interface.setMotorsTorque(RobotState.motors_tor_out);

            logger.startNewLine();
            logger.recItermData("simTime", simTime);
            logger.recItermData("motors_pos_cur",RobotState.motors_pos_cur);
            logger.recItermData("motors_pos_des",RobotState.motors_pos_des);
            logger.recItermData("motors_tau_cur",RobotState.motors_tor_out);
            logger.recItermData("motors_vel_cur",RobotState.motors_vel_cur);
            logger.recItermData("motors_vel_des",RobotState.motors_vel_des);
            logger.finishLine();

            printf("rpyVal=[%.5f, %.5f, %.5f]\n", RobotState.rpy[0], RobotState.rpy[1], RobotState.rpy[2]);
            printf("gps=[%.5f, %.5f, %.5f]\n", RobotState.basePos[0], RobotState.basePos[1], RobotState.basePos[2]);
            printf("vel=[%.5f, %.5f, %.5f]\n", RobotState.baseLinVel[0], RobotState.baseLinVel[1], RobotState.baseLinVel[2]);
        }

        if (mj_data->time>=simEndTime)
        {
            break;
        }

        uiController.updateScene();
    }

//    // free visualization storage
    uiController.Close();

    // free MuJoCo model and data, deactivate
    mj_deleteData(mj_data);
    mj_deleteModel(mj_model);

    return 0;
}