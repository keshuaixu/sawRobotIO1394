/* -*- Mode: C++; tab-width: 4; indent-tabs-mode: nil; c-basic-offset: 4 -*-    */
/* ex: set filetype=cpp softtabstop=4 shiftwidth=4 tabstop=4 cindent expandtab: */

/*
 $Id$
 
 Author(s):  Zihan Chen, Peter Kazanzides
 Created on: 2012-07-31
 
 (C) Copyright 2011-2013 Johns Hopkins University (JHU), All Rights Reserved.
 
 --- begin cisst license - do not edit ---
 
 This software is provided "as is" under an open source license, with
 no warranty.  The complete license can be found in license.txt and
 http://www.cisst.org/cisst/license.txt.
 
 --- end cisst license ---
 */

#include <iostream>

#include <cisstCommon/cmnTokenizer.h>   // TEMP
#include <cisstMultiTask/mtsInterfaceProvided.h>

#include <sawRobotIO1394/mtsRobotIO1394.h>

#include "FirewirePort.h"
#include "AmpIO.h"
#include "mtsRobotInternal.h"

class mtsRobotIO1394::BoardInfo : public AmpIO {
    bool safetyRelayControl;
    bool powerControl;

public:
    BoardInfo(unsigned int board_id) : AmpIO(board_id), 
                                          safetyRelayControl(false), powerControl(false) {}
    ~BoardInfo() {}

    void InitControl(void) {
        safetyRelayControl = false;
        powerControl = false;
    }
    // Do quad read/write for power, safety relay, if necessary
    void DoControl(void) {} // TBD
};


CMN_IMPLEMENT_SERVICES_DERIVED_ONEARG(mtsRobotIO1394, mtsTaskPeriodic, mtsTaskPeriodicConstructorArg)


//============ mtsRobotIO1394 =========================================

mtsRobotIO1394::mtsRobotIO1394(const std::string &name, double period, int port_num)
    : mtsTaskPeriodic(name, period)
{
    Init();
    Port = new FirewirePort(port_num);
}

mtsRobotIO1394::mtsRobotIO1394(const mtsTaskPeriodicConstructorArg &arg)
    : mtsTaskPeriodic(arg)
{
    Init();
    Port = new FirewirePort(0);
}

mtsRobotIO1394::~mtsRobotIO1394()
{
    // Delete boards
    for(int i = 0; i < mtsRobotIO1394::MAX_BOARDS; i++) {
        if (BoardList[i]) {
            Port->RemoveBoard(i);
            delete BoardList[i];
        }
    }
    delete Port;
}

void mtsRobotIO1394::Init(void)
{
    for (int i = 0; i < mtsRobotIO1394::MAX_BOARDS; i++)
        BoardList[i] = 0;

    mtsInterfaceProvided* prov = AddInterfaceProvided("MainInterface");
    if (prov) {
        prov->AddCommandRead(&mtsRobotIO1394::GetNumberOfBoards, this, "GetNumberOfBoards");
        prov->AddCommandRead(&mtsRobotIO1394::GetNumberOfRobots, this, "GetNumberOfRobots");
    }
}

void mtsRobotIO1394::Configure(const std::string &filename)
{
    CMN_LOG_CLASS_INIT_VERBOSE << "Configuring from " << filename << std::endl;

    // For now, use filename to specify pairs of boards.
    // In the future, change this to be an XML file
    cmnTokenizer tokens;
    tokens.Parse(filename);

    for (unsigned int i = 0; i < tokens.GetNumTokens()-1; i += 2) {
        int bd1 = atoi(tokens.GetToken(i));
        BoardList[bd1] = new mtsRobotIO1394::BoardInfo(bd1);
        Port->AddBoard(BoardList[bd1]);
        int bd2 = atoi(tokens.GetToken(i+1));
        BoardList[bd2] = new mtsRobotIO1394::BoardInfo(bd2);
        Port->AddBoard(BoardList[bd2]);
        std::string name("Robot_");
        name.append(tokens.GetToken(i));
        name.append("_");
        name.append(tokens.GetToken(i+1));
        RobotInternal *robot = new RobotInternal(name, 8);
        for (int j = 0; j < 4; j++) {
            robot->JointList[j] = RobotInternal::JointInfo(bd1, j);
            robot->JointList[j+4] = RobotInternal::JointInfo(bd2, j);
        }
        robot->SetupStateTable(StateTable);
        mtsInterfaceProvided* prov = AddInterfaceProvided(name);
        robot->SetupProvidedInterface(prov, StateTable);
        RobotList.push_back(robot);
    }
}

void mtsRobotIO1394::Run(void)
{
    size_t i, j;
    // Initialize power and relay control
    for (i = 0; i < mtsRobotIO1394::MAX_BOARDS; i++)
        if (BoardList[i]) BoardList[i]->InitControl();
    Port->ReadAllBoards();
    // First, check which robots have valid data
    for (i = 0; i < RobotList.size(); i++) {
        for (j = 0; j < RobotList[i]->JointList.size(); j++) {
            RobotInternal::JointInfo &jt = RobotList[i]->JointList[j];
            if ((jt.boardid < 0) || (jt.axisid < 0)) break;  // should not happen
            if (!BoardList[jt.boardid]) break;   // should not happen
            if (!BoardList[jt.boardid]->ValidRead()) break;
        }
        RobotList[i]->SetValid(j == RobotList[i]->JointList.size());
    }
    // Loop through the valid robots, processing feedback
    for (i = 0; i < RobotList.size(); i++) {
        if (!RobotList[i]->IsValid()) continue;
        for (j = 0; j < RobotList[i]->JointList.size(); j++) {
            RobotInternal::JointInfo &jt = RobotList[i]->JointList[j];
            RobotList[i]->encPosRaw[j] = BoardList[jt.boardid]->GetEncoderPosition(jt.axisid);
            RobotList[i]->encVelRaw[j] = BoardList[jt.boardid]->GetEncoderVelocity(jt.axisid);
            RobotList[i]->analogInRaw[j] = BoardList[jt.boardid]->GetAnalogInput(jt.axisid);
            RobotList[i]->motorFeedbackCurrentRaw[j] = BoardList[jt.boardid]->GetMotorCurrent(jt.axisid);
        }
        // Convert from raw to SI units (TBD)
        RobotList[i]->EncoderToDegree(RobotList[i]->encPosRaw, RobotList[i]->encPos);
        RobotList[i]->EncoderToDegPerSec(RobotList[i]->encVelRaw, RobotList[i]->encVel);
        RobotList[i]->ADCToVolts(RobotList[i]->analogInRaw, RobotList[i]->analogIn);
        RobotList[i]->ADCToMotorCurrent(RobotList[i]->motorFeedbackCurrentRaw, RobotList[i]->motorFeedbackCurrent);
    }
    RunEvent();
    ProcessQueuedCommands();
    // Do power and relay control
    // (TBD)
    // Loop through the valid robots, setting outputs
    for (i = 0; i < RobotList.size(); i++) {
        if (!RobotList[i]->IsValid()) continue;
        for (j = 0; j < RobotList[i]->JointList.size(); j++) {
            RobotInternal::JointInfo &jt = RobotList[i]->JointList[j];
            BoardList[jt.boardid]->SetMotorCurrent(jt.axisid, RobotList[i]->motorControlCurrentRaw[j]);
        }
    }
    Port->WriteAllBoards();
}

void mtsRobotIO1394::GetNumberOfBoards(int &num) const
{
    num = 0;
    for (size_t i = 0; i < mtsRobotIO1394::MAX_BOARDS; i++)
        if (BoardList[i]) num++;
}

void mtsRobotIO1394::GetNumberOfRobots(int &num) const
{
    num = RobotList.size();
}
