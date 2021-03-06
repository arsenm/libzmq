/*
    Copyright (c) 2007-2016 Contributors as noted in the AUTHORS file

    This file is part of libzmq, the ZeroMQ core engine in C++.

    libzmq is free software; you can redistribute it and/or modify it under
    the terms of the GNU Lesser General Public License (LGPL) as published
    by the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.

    As a special exception, the Contributors give you permission to link
    this library with independent modules to produce an executable,
    regardless of the license terms of these independent modules, and to
    copy and distribute the resulting executable under terms of your choice,
    provided that you also meet, for each linked independent module, the
    terms and conditions of the license of that module. An independent
    module is a module which is not derived from or based on this library.
    If you modify this library, you must extend this exception to your
    version of the library.

    libzmq is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE. See the GNU Lesser General Public
    License for more details.

    You should have received a copy of the GNU Lesser General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include "precompiled.hpp"
#include <string.h>

#include "mechanism.hpp"
#include "options.hpp"
#include "msg.hpp"
#include "err.hpp"
#include "wire.hpp"
#include "session_base.hpp"

zmq::mechanism_t::mechanism_t (const options_t &options_) :
    options (options_)
{
}

zmq::mechanism_t::~mechanism_t ()
{
}

void zmq::mechanism_t::set_peer_routing_id (const void *id_ptr, size_t id_size)
{
    routing_id = blob_t (static_cast <const unsigned char*> (id_ptr), id_size);
}

void zmq::mechanism_t::peer_routing_id (msg_t *msg_)
{
    const int rc = msg_->init_size (routing_id.size ());
    errno_assert (rc == 0);
    memcpy (msg_->data (), routing_id.data (), routing_id.size ());
    msg_->set_flags (msg_t::routing_id);
}

void zmq::mechanism_t::set_user_id (const void *data_, size_t size_)
{
    user_id = blob_t (static_cast <const unsigned char*> (data_), size_);
    zap_properties.insert (metadata_t::dict_t::value_type (
      ZMQ_MSG_PROPERTY_USER_ID, std::string ((char *) data_, size_)));
}

zmq::blob_t zmq::mechanism_t::get_user_id () const
{
    return user_id;
}

const char *zmq::mechanism_t::socket_type_string (int socket_type) const
{
    static const char *names [] = {"PAIR", "PUB", "SUB", "REQ", "REP",
                                   "DEALER", "ROUTER", "PULL", "PUSH",
                                   "XPUB", "XSUB", "STREAM",
                                   "SERVER", "CLIENT",
                                   "RADIO", "DISH",
                                   "GATHER", "SCATTER", "DGRAM"};
    zmq_assert (socket_type >= 0 && socket_type <= 18);
    return names [socket_type];
}

static size_t property_len (size_t name_len, size_t value_len)
{
    return 1 + name_len + 4 + value_len;
}

static size_t name_len (const char *name)
{
    const size_t name_len = strlen (name);
    zmq_assert (name_len <= 255);
    return name_len;
}

size_t zmq::mechanism_t::add_property (unsigned char *ptr,
                                       size_t ptr_capacity,
                                       const char *name,
                                       const void *value,
                                       size_t value_len)
{
    const size_t name_len = ::name_len (name);
    const size_t total_len = ::property_len (name_len, value_len);
    zmq_assert (total_len <= ptr_capacity);

    *ptr++ = static_cast <unsigned char> (name_len);
    memcpy (ptr, name, name_len);
    ptr += name_len;
    zmq_assert (value_len <= 0x7FFFFFFF);
    put_uint32 (ptr, static_cast <uint32_t> (value_len));
    ptr += 4;
    memcpy (ptr, value, value_len);

    return total_len;
}

size_t zmq::mechanism_t::property_len (const char *name, size_t value_len)
{
    return ::property_len (name_len (name), value_len);
}

#define ZMTP_PROPERTY_SOCKET_TYPE "Socket-Type"
#define ZMTP_PROPERTY_IDENTITY "Identity"

size_t zmq::mechanism_t::add_basic_properties (unsigned char *buf,
                                               size_t buf_capacity) const
{
    unsigned char *ptr = buf;

    //  Add socket type property
    const char *socket_type = socket_type_string (options.type);
    ptr += add_property (ptr, buf_capacity,
                         ZMTP_PROPERTY_SOCKET_TYPE, socket_type,
                         strlen (socket_type));

    //  Add identity (aka routing id) property
    if (options.type == ZMQ_REQ || options.type == ZMQ_DEALER
        || options.type == ZMQ_ROUTER)
        ptr += add_property (ptr, buf_capacity - (ptr - buf),
                             ZMTP_PROPERTY_IDENTITY, options.routing_id,
                             options.routing_id_size);

    return ptr - buf;
}

size_t zmq::mechanism_t::basic_properties_len() const
{
    const char *socket_type = socket_type_string (options.type);
    return property_len (ZMTP_PROPERTY_SOCKET_TYPE, strlen (socket_type))
           + ((options.type == ZMQ_REQ || options.type == ZMQ_DEALER
               || options.type == ZMQ_ROUTER)
                ? property_len (ZMTP_PROPERTY_IDENTITY,
                                options.routing_id_size)
                : 0);
}

void zmq::mechanism_t::make_command_with_basic_properties (
  msg_t *msg_, const char *prefix, size_t prefix_len) const
{
    const size_t command_size = prefix_len + basic_properties_len ();
    const int rc = msg_->init_size (command_size);
    errno_assert (rc == 0);

    unsigned char *ptr = (unsigned char *) msg_->data ();

    //  Add prefix
    memcpy (ptr, prefix, prefix_len);
    ptr += prefix_len;

    add_basic_properties (
      ptr, command_size - (ptr - (unsigned char *) msg_->data ()));
}

int zmq::mechanism_t::parse_metadata (const unsigned char *ptr_,
                                      size_t length_,
                                      bool zap_flag)
{
    size_t bytes_left = length_;

    while (bytes_left > 1) {
        const size_t name_length = static_cast <size_t> (*ptr_);
        ptr_ += 1;
        bytes_left -= 1;
        if (bytes_left < name_length)
            break;

        const std::string name = std::string ((char *) ptr_, name_length);
        ptr_ += name_length;
        bytes_left -= name_length;
        if (bytes_left < 4)
            break;

        const size_t value_length = static_cast <size_t> (get_uint32 (ptr_));
        ptr_ += 4;
        bytes_left -= 4;
        if (bytes_left < value_length)
            break;

        const uint8_t *value = ptr_;
        ptr_ += value_length;
        bytes_left -= value_length;

        if (name == ZMTP_PROPERTY_IDENTITY && options.recv_routing_id)
            set_peer_routing_id (value, value_length);
        else
        if (name == ZMTP_PROPERTY_SOCKET_TYPE) {
            const std::string socket_type ((char *) value, value_length);
            if (!check_socket_type (socket_type)) {
                errno = EINVAL;
                return -1;
            }
        }
        else {
            const int rc = property (name, value, value_length);
            if (rc == -1)
                return -1;
        }
        if (zap_flag)
            zap_properties.insert (
                metadata_t::dict_t::value_type (
                    name, std::string ((char *) value, value_length)));
        else
            zmtp_properties.insert (
                metadata_t::dict_t::value_type (
                    name, std::string ((char *) value, value_length)));
    }
    if (bytes_left > 0) {
        errno = EPROTO;
        return -1;
    }
    return 0;
}

int zmq::mechanism_t::property (const std::string& /* name_ */,
                                const void * /* value_ */, size_t /* length_ */)
{
    //  Default implementation does not check
    //  property values and returns 0 to signal success.
    return 0;
}

bool zmq::mechanism_t::check_socket_type (const std::string& type_) const
{
    switch (options.type) {
        case ZMQ_REQ:
            return type_ == "REP" || type_ == "ROUTER";
        case ZMQ_REP:
            return type_ == "REQ" || type_ == "DEALER";
        case ZMQ_DEALER:
            return type_ == "REP" || type_ == "DEALER" || type_ == "ROUTER";
        case ZMQ_ROUTER:
            return type_ == "REQ" || type_ == "DEALER" || type_ == "ROUTER";
        case ZMQ_PUSH:
            return type_ == "PULL";
        case ZMQ_PULL:
            return type_ == "PUSH";
        case ZMQ_PUB:
            return type_ == "SUB" || type_ == "XSUB";
        case ZMQ_SUB:
            return type_ == "PUB" || type_ == "XPUB";
        case ZMQ_XPUB:
            return type_ == "SUB" || type_ == "XSUB";
        case ZMQ_XSUB:
            return type_ == "PUB" || type_ == "XPUB";
        case ZMQ_PAIR:
            return type_ == "PAIR";
        case ZMQ_SERVER:
            return type_ == "CLIENT";
        case ZMQ_CLIENT:
            return type_ == "SERVER";
        case ZMQ_RADIO:
            return type_ == "DISH";
        case ZMQ_DISH:
            return type_ == "RADIO";
        case ZMQ_GATHER:
            return type_ == "SCATTER";
        case ZMQ_SCATTER:
            return type_ == "GATHER";
        case ZMQ_DGRAM:
            return type_ == "DGRAM";
        default:
            break;
    }
    return false;
}
