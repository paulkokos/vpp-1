/*
 * Copyright (c) 2017 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#ifndef __VOM_TYPES_H__
#define __VOM_TYPES_H__

#include <array>
#include <vector>

#include <boost/asio/ip/address.hpp>

#include "vom/enum_base.hpp"

/**
 * Convenince wrapper macro for error handling in VAPI sends
 */
#define VAPI_CALL(_stmt)                        \
{                                               \
    vapi_error_e _rv;                           \
    do                                          \
    {                                           \
        _rv = (_stmt);                          \
    } while (VAPI_OK != _rv);                   \
}

namespace VOM
{
    /**
     * There needs to be a strict order in which object types are read from VPP
     *  (at boot time) and replayed to VPP (if VPP restarts). That ordering is
     * defined in this enum types
     */
    enum class dependency_t
    {
        /**
         * Global Configuration has no dependency
         */
        GLOBAL = 0,

        /**
         * interfaces are the root of the dependency graph
         */
        INTERFACE,

        /**
         * Tunnel or virtual interfaces next
         */
        TUNNEL,

        /**
         * next bridge/route-domains in which interfaces can be placed.
         */
        FORWARDING_DOMAIN,

        /**
         * ACLs
         */
        ACL,

        /**
         * Then L2/objects that bind to interfaces, BD, ACLS, etc
         */
        BINDING,
    };

    /**
     * Error codes that VPP will return during a HW write
     */
    struct rc_t: public enum_base<rc_t>
    {
        rc_t(const rc_t &rc) = default;

        /**
         * Destructor
         */
        ~rc_t();

        /**
         * The value un-set
         */
        const static rc_t UNSET;

        /**
         * The HW write/update action was/has not been attempted
         */
        const static rc_t NOOP;

        /**
         * The HW write was successfull
         */
        const static rc_t OK;

        /**
         * HW write is in progress. Also used for the 'want' events
         * that never complete
         */
        const static rc_t INPROGRESS;

        /**
         * HW write reported invalid input
         */
        const static rc_t INVALID;

        /**
         * HW write timedout - VPP did not respond within a timely manner
         */
        const static rc_t TIMEOUT;

        /**
         * Get the rc_t from the VPP API value
         */
        static const rc_t &from_vpp_retval(int32_t rv);
    private:
        /**
         * Constructor
         */
        rc_t(int v, const std::string s);
    };

    /**
     * A type declaration of an interface handle in VPP
     */
    struct handle_t
    {
        /**
         * Constructor
         */
        handle_t(int value);

        /**
         * Constructor
         */
        handle_t();

        /**
         * convert to string format for debug purposes
         */
        std::string to_string() const;

        /**
         * Comparison operator
         */
        bool operator==(const handle_t &other) const;

        /**
         * Comparison operator
         */
        bool operator!=(const handle_t &other) const;

        /**
         * less than operator
         */
        bool operator<(const handle_t &other) const;

        /**
         * A value of an interface handle_t that means the itf does not exist
         */
        const static handle_t INVALID;

        /**
         * get the value of the handle
         */
        uint32_t value() const;

    private:
        /**
         * VPP's handle value
         */
        uint32_t m_value;
    };

    /**
     * ostream print of a handle_t
     */
    std::ostream & operator<<(std::ostream &os, const handle_t &h);

    /**
     * Type def of a Ethernet address
     */
    struct mac_address_t
    {
        mac_address_t(uint8_t bytes[6]);
        mac_address_t(std::initializer_list<uint8_t> bytes);
        /**
         * Convert to byte array
         */
        void to_bytes(uint8_t *array, uint8_t len) const;

        /**
         * An all 1's MAC address
         */
        const static mac_address_t ONE;

        /**
         * An all 0's MAC address
         */
        const static mac_address_t ZERO;

        /**
         * Comparison operator
         */
        bool operator==(const mac_address_t &m) const;

        /**
         * less than operator
         */
        bool operator<(const mac_address_t &m) const;

        /**
         * String conversion
         */
        std::string to_string() const;

        /**
         * U64 conversion
         */
        uint64_t to_u64() const;

        /**
         * Underlying bytes array
         */
        std::array<uint8_t, 6> bytes;
    };

    /**
     * Type def of a L2 address as read from VPP
     */
    struct l2_address_t
    {
        l2_address_t(const uint8_t bytes[8], uint8_t n_bytes);
        l2_address_t(std::initializer_list<uint8_t> bytes);
        l2_address_t(const mac_address_t &mac);

        /**
         * Convert to byte array
         */
        void to_bytes(uint8_t *array, uint8_t len) const;

        /**
         * An all 1's L2 address
         */
        const static l2_address_t ONE;

        /**
         * An all 0's L2 address
         */
        const static l2_address_t ZERO;

        /**
         * Comparison operator
         */
        bool operator==(const l2_address_t &m) const;

        /**
         * Comparison operator
         */
        bool operator!=(const l2_address_t &m) const;

        /**
         * String conversion
         */
        std::string to_string() const;

        /**
         * MAC address conversion
         */
        mac_address_t to_mac() const;

        /**
         * Underlying bytes array - filled from least to most significant
         */
        std::vector<uint8_t> bytes;
    };

    /**
     * Ostream operator for a MAC address
     */
    std::ostream &operator<<(std::ostream &os, const mac_address_t &mac);

    /**
     * Ostream operator for a MAC address
     */
    std::ostream &operator<<(std::ostream &os, const l2_address_t &l2);
};

#endif
