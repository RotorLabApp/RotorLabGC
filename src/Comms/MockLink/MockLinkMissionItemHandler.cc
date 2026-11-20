/****************************************************************************
 *
 * (c) 2009-2024 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

#include "MockLinkMissionItemHandler.h"

#include "MAVLinkProtocol.h"
#include "MockLink.h"
#include "QGCLoggingCategory.h"

QGC_LOGGING_CATEGORY(MockLinkMissionItemHandlerLog, "qgc.comms.mocklink.mocklinkmissionitemhandler")

MockLinkMissionItemHandler::MockLinkMissionItemHandler(MockLink *mockLink)
    : QObject(mockLink)
    , _mockLink(mockLink)
{
    // qCDebug(MockLinkMissionItemHandlerLog) << Q_FUNC_INFO << this;

    Q_ASSERT(mockLink);

    (void) connect(&_missionItemResponseTimer, &QTimer::timeout, this, &MockLinkMissionItemHandler::_missionItemResponseTimeout);
}

MockLinkMissionItemHandler::~MockLinkMissionItemHandler()
{
    // qCDebug(MockLinkMissionItemHandlerLog) << Q_FUNC_INFO << this;
}

void MockLinkMissionItemHandler::_startMissionItemResponseTimer()
{
    _missionItemResponseTimer.start(500);
}

bool MockLinkMissionItemHandler::handleMessage(const mavlink_message_t &msg)
{
    switch (msg.msgid) {
    case MAVLINK_MSG_ID_MISSION_REQUEST_LIST:
        _handleMissionRequestList(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_REQUEST_INT:
        _handleMissionRequest(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_ITEM_INT:
        _handleMissionItem(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_COUNT:
        _handleMissionCount(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_ACK:
        // Acks are received back for each MISSION_ITEM message
        _handleMissionAck(msg);
        break;
    case MAVLINK_MSG_ID_MISSION_SET_CURRENT:
        // Sets the currently active mission item
        break;
    case MAVLINK_MSG_ID_MISSION_CLEAR_ALL:
        _handleMissionClearAll(msg);
        break;
    default:
        return false;
    }

    return true;
}

void MockLinkMissionItemHandler::_handleMissionClearAll(const mavlink_message_t &msg)
{
    mavlink_mission_clear_all_t clearAll{};
    mavlink_msg_mission_clear_all_decode(&msg, &clearAll);

    Q_ASSERT(clearAll.target_system == _mockLink->vehicleId());

    _requestType = static_cast<MAV_MISSION_TYPE>(clearAll.mission_type);
    qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionClearAll" << _requestType;

    switch (_requestType) {
    case MAV_MISSION_TYPE_MISSION:
        _missionItems.clear();
        break;
    case MAV_MISSION_TYPE_FENCE:
        _fenceItems.clear();
        break;
    case MAV_MISSION_TYPE_RALLY:
        _rallyItems.clear();
        break;
    case MAV_MISSION_TYPE_ALL:
        _missionItems.clear();
        _fenceItems.clear();
        _rallyItems.clear();
        break;
    default:
        Q_ASSERT(false);
    }

    _sendAck(MAV_MISSION_ACCEPTED);
}

void MockLinkMissionItemHandler::_handleMissionRequestList(const mavlink_message_t &msg)
{
    qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequestList read sequence";

    _failReadRequest1FirstResponse = true;

    if (_failureMode == FailReadRequestListNoResponse) {
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequestList not responding due to failure mode FailReadRequestListNoResponse";
        return;
    }

    if ((_failureMode == FailReadRequestListFirstResponse) && _failReadRequestListFirstResponse) {
        _failReadRequestListFirstResponse = false;
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequestList not responding due to failure mode FailReadRequestListFirstResponse";
        return;
    }

    _failReadRequestListFirstResponse = true;

    mavlink_mission_request_list_t request{};
    mavlink_msg_mission_request_list_decode(&msg, &request);

    Q_ASSERT(request.target_system == _mockLink->vehicleId());

    _requestType = static_cast<MAV_MISSION_TYPE>(request.mission_type);

    int itemCount;
    switch (_requestType) {
    case MAV_MISSION_TYPE_MISSION:
        itemCount = _missionItems.count();
        if (itemCount == 0 && _sendHomePositionOnEmptyList) {
            itemCount = 1;
        }
        break;
    case MAV_MISSION_TYPE_FENCE:
        itemCount = _fenceItems.count();
        break;
    case MAV_MISSION_TYPE_RALLY:
        itemCount = _rallyItems.count();
        break;
    default:
        Q_ASSERT(false);
    }

    mavlink_message_t responseMsg{};
    (void) mavlink_msg_mission_count_pack_chan(
        _mockLink->vehicleId(),
        MAV_COMP_ID_AUTOPILOT1,
        _mockLink->mavlinkChannel(),
        &responseMsg,               // Outgoing message
        msg.sysid,                  // Target is original sender
        msg.compid,                 // Target is original sender
        itemCount,                  // Number of mission items
        _requestType,
        0
    );

    _mockLink->respondWithMavlinkMessage(responseMsg);
}

void MockLinkMissionItemHandler::_handleMissionRequest(const mavlink_message_t &msg)
{
    qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequest read sequence";

    mavlink_mission_request_int_t request{};
    mavlink_msg_mission_request_int_decode(&msg, &request);

    Q_ASSERT(request.target_system == _mockLink->vehicleId());

    if ((_failureMode == FailReadRequest0NoResponse) && (request.seq == 0)) {
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequest not responding due to failure mode FailReadRequest0NoResponse";
        return;
    }

    if ((_failureMode == FailReadRequest1NoResponse) && (request.seq == 1)) {
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequest not responding due to failure mode FailReadRequest1NoResponse";
        return;
    }

    if ((_failureMode == FailReadRequest1FirstResponse) && (request.seq == 1) && _failReadRequest1FirstResponse) {
        _failReadRequest1FirstResponse = false;
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequest not responding due to failure mode FailReadRequest1FirstResponse";
        return;
    }

    // FIXME: Track whether all items are requested, or requested in sequence

    if (((_failureMode == FailReadRequest0IncorrectSequence) && (request.seq == 0)) ||
            ((_failureMode == FailReadRequest1IncorrectSequence) && (request.seq == 1))) {
        // Send back the incorrect sequence number
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionRequest sending bad sequence number";
        request.seq++;
    }

    if (((_failureMode == FailReadRequest0ErrorAck) && (request.seq == 0)) ||
            ((_failureMode == FailReadRequest1ErrorAck) && (request.seq == 1))) {
        _sendAck(_failureAckResult);
        return;
    }

    mavlink_mission_item_int_t missionItemInt{};

    switch (request.mission_type) {
    case MAV_MISSION_TYPE_MISSION:
        if (_missionItems.isEmpty() && _sendHomePositionOnEmptyList) {
            (void) memset(&missionItemInt, 0, sizeof(missionItemInt));
            missionItemInt.frame = MAV_FRAME_GLOBAL_RELATIVE_ALT;
            missionItemInt.command = MAV_CMD_NAV_WAYPOINT;
            missionItemInt.autocontinue = true;
        } else {
            missionItemInt = _missionItems[request.seq];
        }
        break;
    case MAV_MISSION_TYPE_FENCE:
        missionItemInt = _fenceItems[request.seq];
        break;
    case MAV_MISSION_TYPE_RALLY:
        missionItemInt = _rallyItems[request.seq];
        break;
    default:
        Q_ASSERT(false);
    }

    mavlink_message_t responseMsg{};
    (void) mavlink_msg_mission_item_int_pack_chan(
        _mockLink->vehicleId(),
        MAV_COMP_ID_AUTOPILOT1,
        _mockLink->mavlinkChannel(),
        &responseMsg,                // Outgoing message
        msg.sysid,                   // Target is original sender
        msg.compid,                  // Target is original sender
        request.seq,                 // Index of mission item being sent
        missionItemInt.frame,
        missionItemInt.command,
        missionItemInt.current,
        missionItemInt.autocontinue,
        missionItemInt.param1, missionItemInt.param2, missionItemInt.param3, missionItemInt.param4,
        missionItemInt.x, missionItemInt.y, missionItemInt.z,
        _requestType
    );

    _mockLink->respondWithMavlinkMessage(responseMsg);
}

void MockLinkMissionItemHandler::_handleMissionCount(const mavlink_message_t &msg)
{
    mavlink_mission_count_t missionCount{};
    mavlink_msg_mission_count_decode(&msg, &missionCount);
    Q_ASSERT(missionCount.target_system == _mockLink->vehicleId());

    _missionCountCount++;

    if (_staleTransferType != MAV_MISSION_TYPE_ENUM_END) {
        // PX4 v1.15.4 mavlink_mission.cpp handle_mission_count, state GETLIST with _transfer_seq > 0:
        // "WPM: REJ. CMD: Busy" and MISSION_ACK MAV_MISSION_ERROR with the open transfer's mission_type.
        // The reject goes to the partner of the open transfer (send_mission_ack(_transfer_partner_sysid, ...)), not to the sender.
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionCount busy with stale transfer of type:partner" << _staleTransferType << _stalePartnerSystemId << _stalePartnerComponentId;
        _sendAckForType(MAV_MISSION_ERROR, static_cast<MAV_MISSION_TYPE>(_staleTransferType.load()),
                        static_cast<uint8_t>(_stalePartnerSystemId.load()), static_cast<uint8_t>(_stalePartnerComponentId.load()));
        return;
    }

    _requestType = (MAV_MISSION_TYPE)missionCount.mission_type;
    _writeSequenceCount = missionCount.count;
    Q_ASSERT(_writeSequenceCount >= 0);

    qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionCount write sequence _writeSequenceCount:" << _writeSequenceCount;

    switch (missionCount.mission_type) {
    case MAV_MISSION_TYPE_MISSION:
        _missionItems.clear();
        break;
    case MAV_MISSION_TYPE_FENCE:
        _fenceItems.clear();
        break;
    case MAV_MISSION_TYPE_RALLY:
        _rallyItems.clear();
        break;
    }

    if (_writeSequenceCount == 0) {
        if (_failureMode == FailWriteEmptyListFirstAckLost && _failWriteEmptyListFirstAck) {
            _failWriteEmptyListFirstAck = false;
            qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionCount not acking count 0 due to failure mode FailWriteEmptyListFirstAckLost";
            return;
        }
        _sendAck(MAV_MISSION_ACCEPTED);
        return;
    }

    if (_failureMode == FailWriteMissionCountNoResponse) {
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionCount not responding due to failure mode FailWriteMissionCountNoResponse";
        return;
    }

    if (_failureMode == FailWriteMissionCountFirstResponse && _failWriteMissionCountFirstResponse) {
        _failWriteMissionCountFirstResponse = false;
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionCount not responding due to failure mode FailWriteMissionCountNoResponse";
        return;
    }

    _failWriteMissionCountFirstResponse = true;
    _writeSequenceIndex = 0;
    _requestNextMissionItem(_writeSequenceIndex);
}

void MockLinkMissionItemHandler::_requestNextMissionItem(int sequenceNumber)
{
    qCDebug(MockLinkMissionItemHandlerLog) << "write sequence sequenceNumber:" << sequenceNumber << "_failureMode:" << _failureMode;

    if ((_failureMode == FailWriteRequest1NoResponse) && (sequenceNumber == 1)) {
        qCDebug(MockLinkMissionItemHandlerLog) << "_requestNextMissionItem not responding due to failure mode FailWriteRequest1NoResponse";
        return;
    }

    if ((_failureMode == FailWriteRequest1NoResponseStaysBusy) && (sequenceNumber == 1)) {
        // PX4 stays in MAVLINK_WPM_STATE_GETLIST with seq 1 until cancelled or its 5 s timeout
        qCDebug(MockLinkMissionItemHandlerLog) << "_requestNextMissionItem not responding, upload stays open, FailWriteRequest1NoResponseStaysBusy";
        _stalePartnerSystemId = MAVLinkProtocol::instance()->getSystemId();
        _stalePartnerComponentId = MAVLinkProtocol::getComponentId();
        _staleTransferType = _requestType;
        return;
    }

    if ((_failureMode == FailWriteRequest1Delayed) && (sequenceNumber == 1) && _failWriteRequest1Delay) {
        _failWriteRequest1Delay = false;
        qCDebug(MockLinkMissionItemHandlerLog) << "_requestNextMissionItem delaying MISSION_REQUEST 1 due to failure mode FailWriteRequest1Delayed";
        QTimer::singleShot(_writeRequestDelayMs, this, [this]() { _requestNextMissionItem(1); });
        return;
    }

    if (sequenceNumber >= _writeSequenceCount) {
        qCWarning(MockLinkMissionItemHandlerLog) << "_requestNextMissionItem requested seqeuence number > write count sequenceNumber::_writeSequenceCount" << sequenceNumber << _writeSequenceCount;
        return;
    }

    if (((_failureMode == FailWriteRequest0IncorrectSequence) && (sequenceNumber == 0)) ||
            ((_failureMode == FailWriteRequest1IncorrectSequence) && (sequenceNumber == 1))) {
        sequenceNumber++;
    }

    if (((_failureMode == FailWriteRequest0ErrorAck) && (sequenceNumber == 0)) ||
            ((_failureMode == FailWriteRequest1ErrorAck) && (sequenceNumber == 1))) {
        qCDebug(MockLinkMissionItemHandlerLog) << "_requestNextMissionItem sending ack error due to failure mode";
        _sendAck(_failureAckResult);
        return;
    }

    mavlink_message_t message{};
    (void) mavlink_msg_mission_request_int_pack_chan(
        _mockLink->vehicleId(),
        MAV_COMP_ID_AUTOPILOT1,
        _mockLink->mavlinkChannel(),
        &message,
        MAVLinkProtocol::instance()->getSystemId(),
        MAVLinkProtocol::getComponentId(),
        sequenceNumber,
        _requestType
    );
    _mockLink->respondWithMavlinkMessage(message);

    // If response with Mission Item doesn't come before timer fires it's an error
    _startMissionItemResponseTimer();
}

void MockLinkMissionItemHandler::_handleMissionAck(const mavlink_message_t &msg)
{
    mavlink_mission_ack_t missionAck{};
    mavlink_msg_mission_ack_decode(&msg, &missionAck);

    if (missionAck.type == MAV_MISSION_OPERATION_CANCELLED) {
        if (_dropNextCancel) {
            _dropNextCancel = false;
            qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionAck cancel lost for mission_type" << missionAck.mission_type;
            return;
        }
        _cancelAckCount++;
        qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionAck cancel received for mission_type:from" << missionAck.mission_type << msg.sysid << msg.compid;
        _missionItemResponseTimer.stop();
        // PX4: only the transfer partner may cancel (otherwise "REJ. WP CMD: partner id mismatch"); then
        // GETLIST + MAV_MISSION_OPERATION_CANCELLED -> switch_to_idle_state, _transfer_in_progress = false
        if ((missionAck.mission_type == _staleTransferType) && (msg.sysid == _stalePartnerSystemId) && (msg.compid == _stalePartnerComponentId)) {
            _staleTransferType = MAV_MISSION_TYPE_ENUM_END;
        }
    }
}

void MockLinkMissionItemHandler::setBusyStaleTransfer(MAV_MISSION_TYPE staleType, int partnerSystemId, int partnerComponentId)
{
    _stalePartnerSystemId = (partnerSystemId < 0) ? MAVLinkProtocol::instance()->getSystemId() : partnerSystemId;
    _stalePartnerComponentId = (partnerComponentId < 0) ? MAVLinkProtocol::getComponentId() : partnerComponentId;
    _staleTransferType = staleType;
}

void MockLinkMissionItemHandler::_sendAckForType(MAV_MISSION_RESULT ackType, MAV_MISSION_TYPE missionType, uint8_t targetSystem, uint8_t targetComponent) const
{
    mavlink_message_t message{};
    (void) mavlink_msg_mission_ack_pack_chan(
        _mockLink->vehicleId(),
        MAV_COMP_ID_AUTOPILOT1,
        _mockLink->mavlinkChannel(),
        &message,
        targetSystem,
        targetComponent,
        ackType,
        missionType,
        0
    );

    _mockLink->respondWithMavlinkMessage(message);
}

void MockLinkMissionItemHandler::_sendAck(MAV_MISSION_RESULT ackType) const
{
    qCDebug(MockLinkMissionItemHandlerLog) << "_sendAck write sequence complete ackType:" << ackType;

    mavlink_message_t message{};
    (void) mavlink_msg_mission_ack_pack_chan(
        _mockLink->vehicleId(),
        MAV_COMP_ID_AUTOPILOT1,
        _mockLink->mavlinkChannel(),
        &message,
        MAVLinkProtocol::instance()->getSystemId(),
        MAVLinkProtocol::getComponentId(),
        ackType,
        _requestType,
        0
    );

    _mockLink->respondWithMavlinkMessage(message);
}

void MockLinkMissionItemHandler::_handleMissionItem(const mavlink_message_t &msg)
{
    qCDebug(MockLinkMissionItemHandlerLog) << "_handleMissionItem write sequence";

    _missionItemResponseTimer.stop();

    mavlink_mission_item_int_t missionItemInt{};
    mavlink_msg_mission_item_int_decode(&msg, &missionItemInt);

    const MAV_MISSION_TYPE missionType = static_cast<MAV_MISSION_TYPE>(missionItemInt.mission_type);
    const uint16_t seq = missionItemInt.seq;

    switch (missionType) {
    case MAV_MISSION_TYPE_MISSION:
        _missionItems[seq] = missionItemInt;
        break;
    case MAV_MISSION_TYPE_FENCE:
        _fenceItems[seq] = missionItemInt;
        break;
    case MAV_MISSION_TYPE_RALLY:
        _rallyItems[seq] = missionItemInt;
        break;
    case MAV_MISSION_TYPE_ENUM_END:
    case MAV_MISSION_TYPE_ALL:
        qCWarning(MockLinkMissionItemHandlerLog) << "Internal error";
        break;
    }

    _writeSequenceIndex++;
    if (_writeSequenceIndex < _writeSequenceCount) {
        if ((_failureMode == FailWriteFinalAckMissingRequests) && (_writeSequenceIndex == 3)) {
            // Send MAV_MISSION_ACCEPTED ack too early
            _sendAck(MAV_MISSION_ACCEPTED);
        } else {
            _requestNextMissionItem(_writeSequenceIndex);
        }

        return;
    }

    if (_failureMode != FailWriteFinalAckNoResponse) {
        MAV_MISSION_RESULT ack = MAV_MISSION_ACCEPTED;

        if (_failureMode == FailWriteFinalAckErrorAck) {
            ack = MAV_MISSION_ERROR;
        }

        _sendAck(ack);
    }
}

void MockLinkMissionItemHandler::_missionItemResponseTimeout()
{
    qCWarning(MockLinkMissionItemHandlerLog) << "Timeout waiting for next MISSION_ITEM_INT";
    Q_ASSERT(false);
}

void MockLinkMissionItemHandler::sendUnexpectedMissionAck(MAV_MISSION_RESULT ackType)
{
    _sendAck(ackType);
}

void MockLinkMissionItemHandler::sendUnexpectedMissionItem()
{
    // FIXME: NYI
    Q_ASSERT(false);
}

void MockLinkMissionItemHandler::sendUnexpectedMissionRequest()
{
    // FIXME: NYI
    Q_ASSERT(false);
}

void MockLinkMissionItemHandler::setFailureMode(FailureMode_t failureMode, MAV_MISSION_RESULT failureAckResult)
{
    _failureMode = failureMode;
    _failureAckResult = failureAckResult;
    _failWriteEmptyListFirstAck = true;
    _failWriteRequest1Delay = true;
}

void MockLinkMissionItemHandler::shutdown()
{
    _missionItemResponseTimer.stop();
}
