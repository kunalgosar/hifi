//
//  Connection.cpp
//  libraries/networking/src/udt
//
//  Created by Clement on 7/27/15.
//  Copyright 2015 High Fidelity, Inc.
//
//  Distributed under the Apache License, Version 2.0.
//  See the accompanying file LICENSE or http://www.apache.org/licenses/LICENSE-2.0.html
//

#include "Connection.h"

#include <QtCore/QThread>

#include <NumericalConstants.h>

#include "../HifiSockAddr.h"
#include "CongestionControl.h"
#include "ControlPacket.h"
#include "Packet.h"
#include "Socket.h"

using namespace udt;
using namespace std;
using namespace std::chrono;

Connection::Connection(Socket* parentSocket, HifiSockAddr destination, unique_ptr<CongestionControl> congestionControl) :
    _parentSocket(parentSocket),
    _destination(destination),
    _congestionControl(move(congestionControl))
{
    // setup default SYN, RTT and RTT Variance based on the SYN interval in CongestionControl object
    _synInterval = _congestionControl->synInterval();
    _rtt = _synInterval * 10;
    _rttVariance = _rtt / 2;
}

Connection::~Connection() {
    if (_sendQueue) {
        // tell our send queue to stop and wait until its send thread is done
        QThread* sendQueueThread = _sendQueue->thread();
        
        _sendQueue->stop();
        _sendQueue->deleteLater();
        
        sendQueueThread->quit();
        sendQueueThread->wait();
    }
}

void Connection::sendReliablePacket(unique_ptr<Packet> packet) {
    Q_ASSERT_X(packet->isReliable(), "Connection::send", "Trying to send an unreliable packet reliably.");
    
    if (!_sendQueue) {
        // Lasily create send queue
        _sendQueue = SendQueue::create(_parentSocket, _destination);
    }
    
    _sendQueue->queuePacket(move(packet));
}

void Connection::sync() {
    // we send out a periodic ACK every rate control interval
    sendACK();
    
    // check if we need to re-transmit a loss list
    // we do this if it has been longer than the current nakInterval since we last sent
    auto now = high_resolution_clock::now();

    if (duration_cast<microseconds>(now - _lastNAKTime).count() >= _nakInterval) {
        // Send a timeout NAK packet
        sendTimeoutNAK();
    }
}

void Connection::sendACK(bool wasCausedBySyncTimeout) {
    static high_resolution_clock::time_point lastACKSendTime;
    auto currentTime = high_resolution_clock::now();
    
    SequenceNumber nextACKNumber = nextACK();
    Q_ASSERT_X(nextACKNumber < _lastSentACK, "Connection::sendACK", "Sending lower ACK, something is wrong");
    
    if (nextACKNumber == _lastSentACK) {
        // We already sent this ACK, but check if we should re-send it.
        if (nextACKNumber <= _lastReceivedAcknowledgedACK) {
            // we already got an ACK2 for this ACK we would be sending, don't bother
            return;
        }
        
        // We will re-send if it has been more than the estimated timeout since the last ACK
        microseconds sinceLastACK = duration_cast<microseconds>(currentTime - lastACKSendTime);
        
        if (sinceLastACK.count() < estimatedTimeout()) {
            return;
        }
    }
    // we have received new packets since the last sent ACK
    
    // update the last sent ACK
    _lastSentACK = nextACKNumber;
    
    // remove the ACKed packets from the receive queue
    // TODO?
    
    // setup the ACK packet, make it static so we can re-use it
    static const int ACK_PACKET_PAYLOAD_BYTES = sizeof(_lastSentACK) + sizeof(_currentACKSubSequenceNumber)
                                                + sizeof(_rtt) + sizeof(int32_t) + sizeof(int32_t);
    static auto ackPacket = ControlPacket::create(ControlPacket::ACK, ACK_PACKET_PAYLOAD_BYTES);
    ackPacket->reset(); // We need to reset it every time.
    
    // pack in the ACK sub-sequence number
    ackPacket->writePrimitive(_currentACKSubSequenceNumber++);
    
    // pack in the ACK number
    ackPacket->writePrimitive(nextACKNumber);
    
    // pack in the RTT and variance
    ackPacket->writePrimitive(_rtt);
    
    // pack the available buffer size, in packets
    // in our implementation we have no hard limit on receive buffer size, send the default value
    ackPacket->writePrimitive((int32_t) udt::CONNECTION_RECEIVE_BUFFER_SIZE_PACKETS);
    
    if (wasCausedBySyncTimeout) {
        // pack in the receive speed and estimatedBandwidth
        ackPacket->writePrimitive(_receiveWindow.getPacketReceiveSpeed());
        ackPacket->writePrimitive(_receiveWindow.getEstimatedBandwidth());
        
        // record this as the last ACK send time
        lastACKSendTime = high_resolution_clock::now();
    }
    
    // have the send queue send off our packet
    _sendQueue->sendPacket(*ackPacket);
    
    // write this ACK to the map of sent ACKs
    _sentACKs[_currentACKSubSequenceNumber] = { nextACKNumber, currentTime };
    
    // reset the number of data packets received since last ACK
    _packetsSinceACK = 0;
    
    _stats.recordSentACK();
}

void Connection::sendLightACK() {
    SequenceNumber nextACKNumber = nextACK();
    
    if (nextACKNumber == _lastReceivedAcknowledgedACK) {
        // we already got an ACK2 for this ACK we would be sending, don't bother
        return;
    }
    
    // create the light ACK packet, make it static so we can re-use it
    static const int LIGHT_ACK_PACKET_PAYLOAD_BYTES = sizeof(SequenceNumber);
    static auto lightACKPacket = ControlPacket::create(ControlPacket::ACK, LIGHT_ACK_PACKET_PAYLOAD_BYTES);
    
    // reset the lightACKPacket before we go to write the ACK to it
    lightACKPacket->reset();
    
    // pack in the ACK
    lightACKPacket->writePrimitive(nextACKNumber);
    
    // have the send queue send off our packet immediately
    _sendQueue->sendPacket(*lightACKPacket);
    
    _stats.recordSentLightACK();
}

void Connection::sendACK2(SequenceNumber currentACKSubSequenceNumber) {
    // setup a static ACK2 packet we will re-use
    static const int ACK2_PAYLOAD_BYTES = sizeof(SequenceNumber);
    static auto ack2Packet = ControlPacket::create(ControlPacket::ACK2, ACK2_PAYLOAD_BYTES);
    
    // reset the ACK2 Packet before writing the sub-sequence number to it
    ack2Packet->reset();
    
    // write the sub sequence number for this ACK2
    ack2Packet->writePrimitive(currentACKSubSequenceNumber);
    
    // update the last sent ACK2 and the last ACK2 send time
    _lastSentACK2 = currentACKSubSequenceNumber;
    
    _stats.recordSentACK2();
}

void Connection::sendNAK(SequenceNumber sequenceNumberRecieved) {
    // create the loss report packet, make it static so we can re-use it
    static const int NAK_PACKET_PAYLOAD_BYTES = 2 * sizeof(SequenceNumber);
    static auto lossReport = ControlPacket::create(ControlPacket::NAK, NAK_PACKET_PAYLOAD_BYTES);
    lossReport->reset(); // We need to reset it every time.
    
    // pack in the loss report
    lossReport->writePrimitive(_lastReceivedSequenceNumber + 1);
    if (_lastReceivedSequenceNumber + 1 != sequenceNumberRecieved - 1) {
        lossReport->writePrimitive(sequenceNumberRecieved - 1);
    }
    
    // have the send queue send off our packet immediately
    _sendQueue->sendPacket(*lossReport);
    
    // record our last NAK time
    _lastNAKTime = high_resolution_clock::now();
    
    _stats.recordSentNAK();
}

void Connection::sendTimeoutNAK() {
    // construct a NAK packet that will hold all of the lost sequence numbers
    // TODO size is wrong, fix it.
    auto lossListPacket = ControlPacket::create(ControlPacket::TimeoutNAK, _lossList.getLength() * sizeof(SequenceNumber));
    
    // Pack in the lost sequence numbers
    _lossList.write(*lossListPacket);
    
    // have our SendQueue send off this control packet
    _sendQueue->sendPacket(*lossListPacket);
    
    // record this as the last NAK time
    _lastNAKTime = high_resolution_clock::now();
    
    _stats.recordSentTimeoutNAK();
}

SequenceNumber Connection::nextACK() const {
    if (_lossList.getLength() > 0) {
        return _lossList.getFirstSequenceNumber() - 1;
    } else {
        return _lastReceivedSequenceNumber;
    }
}

void Connection::processReceivedSequenceNumber(SequenceNumber seq) {
    
    // check if this is a packet pair we should estimate bandwidth from, or just a regular packet
    if (((uint32_t) seq & 0xF) == 0) {
        _receiveWindow.onProbePair1Arrival();
    } else if (((uint32_t) seq & 0xF) == 1) {
        _receiveWindow.onProbePair2Arrival();
    } else {
        _receiveWindow.onPacketArrival();
    }
    
    // If this is not the next sequence number, report loss
    if (seq > _lastReceivedSequenceNumber + 1) {
        if (_lastReceivedSequenceNumber + 1 == seq - 1) {
            _lossList.append(_lastReceivedSequenceNumber + 1);
        } else {
            _lossList.append(_lastReceivedSequenceNumber + 1, seq - 1);
        }
        
        // Send a NAK packet
        sendNAK(seq);
        
        // figure out when we should send the next loss report, if we haven't heard anything back
        _nakInterval = (_rtt + 4 * _rttVariance);
        
        int receivedPacketsPerSecond = _receiveWindow.getPacketReceiveSpeed();
        if (receivedPacketsPerSecond > 0) {
            // the NAK interval is at least the _minNAKInterval
            // but might be the time required for all lost packets to be retransmitted
            _nakInterval = std::max((int) (_lossList.getLength() * (USECS_PER_SECOND / receivedPacketsPerSecond)),
                                    _minNAKInterval);
        } else {
            // the NAK interval is at least the _minNAKInterval but might be the estimated timeout
            _nakInterval = std::max(estimatedTimeout(), _minNAKInterval);
        }
    }
    
    if (seq > _lastReceivedSequenceNumber) {
        // Update largest recieved sequence number
        _lastReceivedSequenceNumber = seq;
    } else {
        // Otherwise, it's a resend, remove it from the loss list
        _lossList.remove(seq);
    }
    
    // increment the counters for data packets received
    ++_packetsSinceACK;
    ++_totalReceivedDataPackets;
    
    // check if we need to send an ACK, according to CC params
    if (_congestionControl->_ackInterval > 0 && _packetsSinceACK >= _congestionControl->_ackInterval) {
        sendACK(false);
    }
}

void Connection::processControl(unique_ptr<ControlPacket> controlPacket) {
    // Simple dispatch to control packets processing methods based on their type
    switch (controlPacket->getType()) {
        case ControlPacket::ACK:
            if (controlPacket->getPayloadSize() == sizeof(SequenceNumber)) {
                processLightACK(move(controlPacket));
            } else {
                processACK(move(controlPacket));
            }
            break;
        case ControlPacket::ACK2:
            processACK2(move(controlPacket));
            break;
        case ControlPacket::NAK:
            processNAK(move(controlPacket));
            break;
        case ControlPacket::TimeoutNAK:
            processTimeoutNAK(move(controlPacket));
            break;
    }
}

void Connection::processACK(std::unique_ptr<ControlPacket> controlPacket) {
    // read the ACK sub-sequence number
    SequenceNumber currentACKSubSequenceNumber;
    controlPacket->readPrimitive(&currentACKSubSequenceNumber);
    
    // Check if we need send an ACK2 for this ACK
    // This will be the case if it has been longer than the sync interval OR
    // it looks like they haven't received our ACK2 for this ACK
    auto currentTime = high_resolution_clock::now();
    static high_resolution_clock::time_point lastACK2SendTime;
    
    microseconds sinceLastACK2 = duration_cast<microseconds>(currentTime - lastACK2SendTime);
    
    if (sinceLastACK2.count() > _synInterval || currentACKSubSequenceNumber == _lastSentACK2) {
        // Send ACK2 packet
        sendACK2(currentACKSubSequenceNumber);
        
        lastACK2SendTime = high_resolution_clock::now();
    }
    
    // read the ACKed sequence number
    SequenceNumber ack;
    controlPacket->readPrimitive(&ack);
    
    // validate that this isn't a BS ACK
    if (ack > _sendQueue->getCurrentSequenceNumber()) {
        // in UDT they specifically break the connection here - do we want to do anything?
        return;
    }
    
    // read the RTT
    int32_t rtt;
    controlPacket->readPrimitive(&rtt);
    
    // read the desired flow window size
    int flowWindowSize;
    controlPacket->readPrimitive(&flowWindowSize);
    
    if (ack <= _lastReceivedACK) {
        // this is a valid ACKed sequence number - update the flow window size and the last received ACK
        _flowWindowSize = flowWindowSize;
        _lastReceivedACK = ack;
    }
    
    // make sure this isn't a repeated ACK
    if (ack <= SequenceNumber(_atomicLastReceivedACK)) {
        return;
    }
    
    // ACK the send queue so it knows what was received
    _sendQueue->ack(ack);
    
    // update the atomic for last received ACK, the send queue uses this to re-transmit
    _atomicLastReceivedACK = (SequenceNumber::Type)_lastReceivedACK;
    
    // update the RTT
    updateRTT(rtt);
    
    // set the RTT for congestion control
    _congestionControl->setRTT(_rtt);
    
    if (controlPacket->getPayloadSize() > (qint64) (sizeof(SequenceNumber) + sizeof(SequenceNumber) + sizeof(rtt))) {
        int32_t receiveRate, bandwidth;
        controlPacket->readPrimitive(&receiveRate);
        controlPacket->readPrimitive(&bandwidth);
        
        // set the delivery rate and bandwidth for congestion control
        // these are calculated using an EWMA
        static const int EMWA_ALPHA_NUMERATOR = 8;
        
        _deliveryRate = (_deliveryRate * (EMWA_ALPHA_NUMERATOR - 1) + _deliveryRate) / EMWA_ALPHA_NUMERATOR;
        _bandwidth = (_bandwidth * (EMWA_ALPHA_NUMERATOR - 1) + _bandwidth) / EMWA_ALPHA_NUMERATOR;
        
        _congestionControl->setReceiveRate(_deliveryRate);
        _congestionControl->setBandwidth(_bandwidth);
    }
    
    // fire the onACK callback for congestion control
    _congestionControl->setSendCurrentSequenceNumber(_sendQueue->getCurrentSequenceNumber());
    _congestionControl->onAck(ack);
    _sendQueue->setPacketSendPeriod(_congestionControl->_packetSendPeriod);
    
    // update the total count of received ACKs
    _stats.recordReceivedACK();
}

void Connection::processLightACK(std::unique_ptr<ControlPacket> controlPacket) {
    // read the ACKed sequence number
    SequenceNumber ack;
    controlPacket->readPrimitive(&ack);
    
    // must be larger than the last received ACK to be processed
    if (ack > _lastReceivedACK) {
        // decrease the flow window size by the offset between the last received ACK and this ACK
        _flowWindowSize -= seqoff(_lastReceivedACK, ack);
    
        // update the last received ACK to the this one
        _lastReceivedACK = ack;
    }
    
    _stats.recordReceivedLightACK();
}

void Connection::processACK2(std::unique_ptr<ControlPacket> controlPacket) {
    // pull the sub sequence number from the packet
    SequenceNumber subSequenceNumber;
    controlPacket->readPrimitive(&subSequenceNumber);

    // check if we had that subsequence number in our map
    auto it = _sentACKs.find(subSequenceNumber);
    if (it != _sentACKs.end()) {
        // update the RTT using the ACK window
        SequenceNumberTimePair& pair = it->second;
        
        // calculate the RTT (time now - time ACK sent)
        auto now = high_resolution_clock::now();
        int rtt = duration_cast<microseconds>(now - pair.second).count();
        
        updateRTT(rtt);
        
        // set the RTT for congestion control
        _congestionControl->setRTT(_rtt);
        
        // update the last ACKed ACK
        if (pair.first > _lastReceivedAcknowledgedACK) {
            _lastReceivedAcknowledgedACK = pair.first;
        }
    }
    
    _stats.recordReceivedACK2();
}

void Connection::processNAK(std::unique_ptr<ControlPacket> controlPacket) {
    // read the loss report
    SequenceNumber start, end;
    controlPacket->readPrimitive(&start);
    
    end = start;
    
    if (controlPacket->bytesLeftToRead() >= (qint64)sizeof(SequenceNumber)) {
        controlPacket->readPrimitive(&end);
    }
    
    // send that off to the send queue so it knows there was loss
    _sendQueue->nak(start, end);
    
    // Tell the congestion control object that there was loss
    _congestionControl->setSendCurrentSequenceNumber(_sendQueue->getCurrentSequenceNumber());
    _congestionControl->onLoss(start, end);
    _sendQueue->setPacketSendPeriod(_congestionControl->_packetSendPeriod);
    
    _stats.recordReceivedNAK();
}

void Connection::processTimeoutNAK(std::unique_ptr<ControlPacket> controlPacket) {
    // Override SendQueue's LossList with the timeout NAK list
    _sendQueue->overrideNAKListFromPacket(*controlPacket);
    
    // we don't tell the congestion control object there was loss here - this matches UDTs implementation
    // a possible improvement would be to tell it which new loss this timeout packet told us about
    
    _stats.recordReceivedTimeoutNAK();
}

void Connection::updateRTT(int rtt) {
    // This updates the RTT using exponential weighted moving average
    // This is the Jacobson's forumla for RTT estimation
    // http://www.mathcs.emory.edu/~cheung/Courses/455/Syllabus/7-transport/Jacobson-88.pdf
    
    // Estimated RTT = (1 - x)(estimatedRTT) + (x)(sampleRTT)
    // (where x = 0.125 via Jacobson)
    
    // Deviation  = (1 - x)(deviation) + x |sampleRTT - estimatedRTT|
    // (where x = 0.25 via Jacobson)
    
    static const int RTT_ESTIMATION_ALPHA_NUMERATOR = 8;
    static const int RTT_ESTIMATION_VARIANCE_ALPHA_NUMERATOR = 4;
   
    _rtt = (_rtt * (RTT_ESTIMATION_ALPHA_NUMERATOR - 1) + rtt) / RTT_ESTIMATION_ALPHA_NUMERATOR;
    
    _rttVariance = (_rttVariance * (RTT_ESTIMATION_VARIANCE_ALPHA_NUMERATOR - 1)
                    + abs(rtt - _rtt)) / RTT_ESTIMATION_VARIANCE_ALPHA_NUMERATOR;
}

int Connection::estimatedTimeout() const {
    return _congestionControl->_userDefinedRto ? _rtt + _rttVariance * 4 : _congestionControl->_rto;
}
