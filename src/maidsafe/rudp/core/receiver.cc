/*  Copyright 2012 MaidSafe.net limited

    This MaidSafe Software is licensed to you under (1) the MaidSafe.net Commercial License,
    version 1.0 or later, or (2) The General Public License (GPL), version 3, depending on which
    licence you accepted on initial access to the Software (the "Licences").

    By contributing code to the MaidSafe Software, or to this project generally, you agree to be
    bound by the terms of the MaidSafe Contributor Agreement, version 1.0, found in the root
    directory of this project at LICENSE, COPYING and CONTRIBUTOR respectively and also
    available at: http://www.novinet.com/license

    Unless required by applicable law or agreed to in writing, the MaidSafe Software distributed
    under the GPL Licence is distributed on an "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS
    OF ANY KIND, either express or implied.

    See the Licences for the specific language governing permissions and limitations relating to
    use of the MaidSafe Software.                                                                 */

// Original author: Christopher M. Kohlhoff (chris at kohlhoff dot com)

#include "maidsafe/rudp/core/receiver.h"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <limits>

#include "boost/assert.hpp"

#include "maidsafe/common/log.h"
#include "maidsafe/common/utils.h"

#include "maidsafe/rudp/core/congestion_control.h"
#include "maidsafe/rudp/core/peer.h"
#include "maidsafe/rudp/core/tick_timer.h"
#include "maidsafe/rudp/packets/ack_of_ack_packet.h"
#include "maidsafe/rudp/packets/negative_ack_packet.h"

namespace asio = boost::asio;
namespace ip = asio::ip;
namespace bptime = boost::posix_time;

namespace maidsafe {

namespace rudp {

namespace detail {

Receiver::Receiver(Peer& peer, TickTimer& tick_timer, CongestionControl& congestion_control)  // NOLINT (Fraser)
    : peer_(peer),
      tick_timer_(tick_timer),
      congestion_control_(congestion_control),
      unread_packets_(),
      acks_(),
      last_ack_packet_sequence_number_(0),
      ack_sent_time_(tick_timer_.Now()) {}

void Receiver::Reset(uint32_t initial_sequence_number) {
  unread_packets_.Reset(initial_sequence_number);
  last_ack_packet_sequence_number_ = initial_sequence_number;
}

bool Receiver::Flushed() const {
  uint32_t ack_packet_seqnum = AckPacketSequenceNumber();
  return acks_.IsEmpty() && (ack_packet_seqnum == last_ack_packet_sequence_number_ ||
                             last_ack_packet_sequence_number_ == 0);
}

size_t Receiver::ReadData(const boost::asio::mutable_buffer& data) {
  unsigned char* begin = asio::buffer_cast<unsigned char*>(data);
  unsigned char* ptr = begin;
  unsigned char* end = begin + asio::buffer_size(data);

  for (uint32_t n = unread_packets_.Begin();
       (n != unread_packets_.End()) && (ptr < end);
       n = unread_packets_.Next(n)) {
    UnreadPacket& p = unread_packets_[n];
//    LOG(kSuccess) << p.packet.MessageNumber() << std::boolalpha << "\t"
//                  << p.packet.FirstPacketInMessage() << "\t" << p.packet.LastPacketInMessage();
    if (p.lost) {
      break;
    } else if (p.packet.Data().size() > p.bytes_read) {
      size_t length = std::min<size_t>(end - ptr, p.packet.Data().size() - p.bytes_read);
      std::memcpy(ptr, p.packet.Data().data() + p.bytes_read, length);
      ptr += length;
      p.bytes_read += length;
      if (p.packet.Data().size() == p.bytes_read) {
        unread_packets_.Remove();
      }
    } else {
      unread_packets_.Remove();
    }
  }

  return ptr - begin;
}

void Receiver::HandleData(const DataPacket& packet) {
  unread_packets_.SetMaximumSize(congestion_control_.ReceiveWindowSize());

  uint32_t seqnum = packet.PacketSequenceNumber();

  // Make sure there is space in the window for packets that are expected soon.
  // sliding_window will keep appending till reach the current seqnum or full.
  // i.e. any un-received packet, having previous seqnum, will be given an empty
  // reserved slot.
  // Later arrvied packet, having less seqnum, will not affect sliding window
  while (unread_packets_.IsComingSoon(seqnum) && !unread_packets_.IsFull())
    // New entries are marked "lost" by default, and reserve_time set to now
    unread_packets_.Append();

  // Ignore any packet which isn't in the window.
  // The empty slot will got populated here, if the packet arrived later having
  // a seqnum falls in the window
  if (unread_packets_.Contains(seqnum)) {
    UnreadPacket& p = unread_packets_[seqnum];
    // The packet will be ignored if already received
    if (p.lost) {
      congestion_control_.OnDataPacketReceived(seqnum);
      p.packet = packet;
      p.lost = false;
      p.bytes_read = 0;
    }
  } else {
    LOG(kWarning) << "Ignoring incoming packet with seqnum " << seqnum
                  << ".\tCurrent unread range is " << unread_packets_.Begin() << " to "
                  << unread_packets_.End();
  }

  if (seqnum % congestion_control_.AckInterval() == 0) {
    // Send acknowledgement packets immediately.
    HandleTick();
  } else {
    // Schedule generation of acknowledgement packets for later.
    tick_timer_.TickAfter(congestion_control_.AckDelay());
  }
//  if (tick_timer_.Expired()) {
//    tick_timer_.TickAfter(congestion_control_.ReceiveDelay());
//  }
}

void Receiver::HandleAckOfAck(const AckOfAckPacket& packet) {
  uint32_t ack_seqnum = packet.AckSequenceNumber();

  if (acks_.Contains(ack_seqnum)) {
    Ack& a = acks_[ack_seqnum];
    boost::posix_time::time_duration rtt = tick_timer_.Now() - a.send_time;
    uint64_t rtt_us = rtt.total_microseconds();
    if (rtt_us < std::numeric_limits<uint32_t>::max()) {
      congestion_control_.OnAckOfAck(static_cast<uint32_t>(rtt_us));
    }
  }

  while (acks_.Contains(ack_seqnum)) {
    acks_.Remove();
  }
}

void Receiver::HandleTick() {
  bptime::ptime now = tick_timer_.Now();

  // Generate an acknowledgement only if the latest sequence number has
  // changed, or if it has been too long since the last unacknowledged
  // acknowledgement.
  AddAckToWindow(now);
  if (!acks_.IsEmpty()) {
    if (acks_.Back().send_time + congestion_control_.AckTimeout() > now) {
      tick_timer_.TickAt(acks_.Back().send_time + congestion_control_.AckTimeout());
    }
  }

  // Generate a negative acknowledgement packet to request missing packets.
  NegativeAckPacket negative_ack;
  negative_ack.SetDestinationSocketId(peer_.SocketId());
  AddMissingSequenceNumbersToNegAck(negative_ack);
  if (negative_ack.HasSequenceNumbers()) {
    peer_.Send(negative_ack);
    tick_timer_.TickAt(now + congestion_control_.AckTimeout());
  }
}

void Receiver::AddAckToWindow(const bptime::ptime& now) {
  // Generate an acknowledgement only if the latest sequence number has
  // changed, or if it has been too long since the last unacknowledged
  // acknowledgement.
  uint32_t ack_packet_seqnum = AckPacketSequenceNumber();
  if ((ack_packet_seqnum != last_ack_packet_sequence_number_) ||
      (!acks_.IsEmpty() && (acks_.Back().send_time + congestion_control_.AckTimeout() <= now))) {
// TODO(Fraser#5#): 2012-09-03 - Confirm this is OK to be removed.
//    if (acks_.IsFull())
//      acks_.Remove();
    congestion_control_.OnGenerateAck(ack_packet_seqnum);
    uint32_t n = acks_.Append();
    Ack& a = acks_[n];
    a.packet.SetDestinationSocketId(peer_.SocketId());
    a.packet.SetAckSequenceNumber(n);
    a.packet.SetPacketSequenceNumber(ack_packet_seqnum);
    a.packet.SetHasOptionalFields(true);
    a.packet.SetRoundTripTime(congestion_control_.RoundTripTime());
    a.packet.SetRoundTripTimeVariance(congestion_control_.RoundTripTimeVariance());
    a.packet.SetAvailableBufferSize(AvailableBufferSize());
    a.packet.SetPacketsReceivingRate(congestion_control_.PacketsReceivingRate());
    a.packet.SetEstimatedLinkCapacity(congestion_control_.EstimatedLinkCapacity());
    a.send_time = now;
    peer_.Send(a.packet);
    last_ack_packet_sequence_number_ = ack_packet_seqnum;
  }
}

void Receiver::AddMissingSequenceNumbersToNegAck(NegativeAckPacket& negative_ack) {
  uint32_t n = unread_packets_.Begin();
  while (n != unread_packets_.End()) {
    if (unread_packets_[n].lost) {
      uint32_t begin = n;
      uint32_t end;
      do {
        end = n;
        n = unread_packets_.Next(n);
      } while (n != unread_packets_.End() && unread_packets_[n].lost);
      if (begin == end)
        negative_ack.AddSequenceNumber(begin);
      else
        negative_ack.AddSequenceNumbers(begin, end);
    } else {
      n = unread_packets_.Next(n);
    }
  }
}

uint32_t Receiver::AvailableBufferSize() const {
  size_t free_packets =
      unread_packets_.IsFull() ? 0 : unread_packets_.MaximumSize() - unread_packets_.Size();
  assert(free_packets * Parameters::max_data_size < std::numeric_limits<uint32_t>::max());
  return static_cast<uint32_t>(free_packets * Parameters::max_data_size);
}

uint32_t Receiver::AckPacketSequenceNumber() const {
  // Work out what sequence number we need to acknowledge up to.
  uint32_t ack_packet_seqnum = unread_packets_.Begin();
  while (ack_packet_seqnum != unread_packets_.End() && !unread_packets_[ack_packet_seqnum].lost)
    ack_packet_seqnum = unread_packets_.Next(ack_packet_seqnum);
  return ack_packet_seqnum;
}

}  // namespace detail

}  // namespace rudp

}  // namespace maidsafe
