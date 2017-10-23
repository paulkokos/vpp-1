/*
 * Copyright (c) 2017 Cisco Systems, Inc. and others.  All rights reserved.
 *
 * This program and the accompanying materials are made available under the
 * terms of the Eclipse Public License v1.0 which accompanies this distribution,
 * and is available at http://www.eclipse.org/legal/epl-v10.html
 */

#ifndef __VOM_ACL_BINDING_H__
#define __VOM_ACL_BINDING_H__

#include <ostream>
#include <string>
#include <stdint.h>

#include "vom/object_base.hpp"
#include "vom/om.hpp"
#include "vom/hw.hpp"
#include "vom/rpc_cmd.hpp"
#include "vom/singular_db.hpp"
#include "vom/acl_types.hpp"
#include "vom/interface.hpp"
#include "vom/acl_list.hpp"
#include "vom/inspect.hpp"

namespace VOM
{
    namespace ACL
    {
        /**
         * A binding between an ACL and an interface.
         * A representation of the application of the ACL to the interface.
         */
        template <typename LIST, typename BIND, typename DUMP>
        class binding: public object_base
        {
        public:
            /**
             * The key for a binding is the direction and the interface
             */
            typedef std::pair<direction_t, interface::key_type> key_t;

            /**
             * Construct a new object matching the desried state
             */
            binding(const direction_t &direction,
                    const interface &itf,
                    const LIST &acl):
                m_direction(direction),
                m_itf(itf.singular()),
                m_acl(acl.singular()),
                m_binding(0)
            {
                m_evh.order();
            }

            /**
             * Copy Constructor
             */
            binding(const binding& o):
                m_direction(o.m_direction),
                m_itf(o.m_itf),
                m_acl(o.m_acl),
                m_binding(0)
            {
            }

            /**
             * Destructor
             */
            ~binding()
            {
                sweep();
                m_db.release(std::make_pair(m_direction, m_itf->key()), this);
            }

            /**
             * Return the 'singular instance' of the L2 config that matches this object
             */
            std::shared_ptr<binding> singular() const
            {
                return find_or_add(*this);
            }

            /**
             * convert to string format for debug purposes
             */
            std::string to_string() const
            {
                std::ostringstream s;
                s << "acl-binding:["
		  << " " << m_direction.to_string()
                  << m_itf->to_string()
                  << " " << m_acl->to_string()
                  << " " << m_binding.to_string()
                  << "]";

                return (s.str());
            }

            /**
             * Dump all bindings into the stream provided
             */
            static void dump(std::ostream &os)
            {
                m_db.dump(os);
            }

            /**
             * A command class that binds the ACL to the interface
             */
            class bind_cmd: public rpc_cmd<HW::item<bool>, rc_t, BIND>
            {
            public:
                /**
                 * Constructor
                 */
                bind_cmd(HW::item<bool> &item,
                        const direction_t &direction,
                        const handle_t &itf,
                        const handle_t &acl):
                    rpc_cmd<HW::item<bool>, rc_t, BIND>(item),
                    m_direction(direction),
                    m_itf(itf),
                    m_acl(acl)
                {
                }

                /**
                 * Issue the command to VPP/HW
                 */
                rc_t issue(connection &con);

                /**
                 * convert to string format for debug purposes
                 */
                std::string to_string() const
                {
                    std::ostringstream s;
                    s << "acl-bind:["
                      << m_direction.to_string()
                      << " itf:" << m_itf.to_string()
                      << " acl:" << m_acl.to_string()
                      << "]";

                    return (s.str());
                }

                /**
                 * Comparison operator - only used for UT
                 */
                bool operator==(const bind_cmd&other) const
                {
                    return ((m_itf == other.m_itf) &&
                            (m_acl == m_acl));
                }

            private:
                /**
                 * The direction of the binding
                 */
                const direction_t m_direction;

                /**
                 * The interface to bind to
                 */
                const handle_t m_itf;

                /**
                 * The ACL to bind
                 */
                const handle_t m_acl;
            };

            /**
             * A command class that binds the ACL to the interface
             */
            class unbind_cmd: public rpc_cmd<HW::item<bool>, rc_t, BIND>
            {
            public:
                /**
                 * Constructor
                 */
                unbind_cmd(HW::item<bool> &item,
                          const direction_t &direction,
                          const handle_t &itf,
                          const handle_t &acl):
                    rpc_cmd<HW::item<bool>, rc_t, BIND>(item),
                    m_direction(direction),
                    m_itf(itf),
                    m_acl(acl)
                {
                }

                /**
                 * Issue the command to VPP/HW
                 */
                rc_t issue(connection &con);

                /**
                 * convert to string format for debug purposes
                 */
                std::string to_string() const
                {
                    std::ostringstream s;
                    s << "acl-unbind:["
                      << m_direction.to_string()
                      << " itf:" << m_itf.to_string()
                      << " acl:" << m_acl.to_string()
                      << "]";

                    return (s.str());
                }

                /**
                 * Comparison operator - only used for UT
                 */
                bool operator==(const unbind_cmd&other) const
                {
                    return ((m_itf == other.m_itf) &&
                            (m_acl == m_acl));
                }

            private:
                /**
                 * The direction of the binding
                 */
                const direction_t m_direction;

                /**
                 * The interface to bind to
                 */
                const handle_t m_itf;

                /**
                 * The ACL to bind
                 */
                const handle_t m_acl;
            };

            /**
             * A cmd class that Dumps all the ACLs
             */
            class dump_cmd: public VOM::dump_cmd<DUMP>
            {
            public:
                /**
                 * Constructor
                 */
                dump_cmd() = default;
                dump_cmd(const dump_cmd &d) = default;

                /**
                 * Issue the command to VPP/HW
                 */
                rc_t issue(connection &con);

                /**
                 * convert to string format for debug purposes
                 */
                std::string to_string() const
                {
                    return ("acl-bind-dump");
                }

            private:
                /**
                 * HW reutrn code
                 */
                HW::item<bool> item;
            };

        private:
            /**
             * Class definition for listeners to OM events
             */
            class event_handler: public OM::listener, public inspect::command_handler
            {
            public:
                event_handler()
                {
                    OM::register_listener(this);
                    inspect::register_handler({"acl-binding"}, "ACL bindings", this);
                }
                virtual ~event_handler() = default;

                /**
                 * Handle a populate event
                 */
                void handle_populate(const client_db::key_t & key);

                /**
                 * Handle a replay event
                 */
                void handle_replay()
                {
                    m_db.replay();
                }

                /**
                 * Show the object in the Singular DB
                 */
                void show(std::ostream &os)
                {
                    m_db.dump(os);
                }

                /**
                 * Get the sortable Id of the listener
                 */
                dependency_t order() const
                {
                    return (dependency_t::BINDING);
                }
            };

            /**
             * event_handler to register with OM
             */
            static event_handler m_evh;

            /**
             * Enquue commonds to the VPP command Q for the update
             */
            void update(const binding &obj)
            {
                if (!m_binding)
                {
                    HW::enqueue(new bind_cmd(m_binding,
                                            m_direction,
                                            m_itf->handle(),
                                            m_acl->handle()));
                }
                HW::write();
            }

            /**
             * Find or Add the instance in the DB
             */
            static std::shared_ptr<binding> find_or_add(const binding &temp)
            {
                return (m_db.find_or_add(std::make_pair(temp.m_direction,
                                                        temp.m_itf->key()),
                                         temp));
            }

            /*
             * It's the VOM::OM class that calls singular()
             */
            friend class VOM::OM;

            /**
             * It's the VOM::singular_db class that calls replay()
             */
            friend class VOM::singular_db<key_t, binding>;

            /**
             * Sweep/reap the object if still stale
             */
            void sweep(void)
            {
                if (m_binding)
                {
                    HW::enqueue(new unbind_cmd(m_binding,
                                              m_direction,
                                              m_itf->handle(),
                                              m_acl->handle()));
                }
                HW::write();
            }

            /**
             * Replay the objects state to HW
             */
            void replay(void)
            {
                if (m_binding)
                {
                    HW::enqueue(new bind_cmd(m_binding,
                                            m_direction,
                                            m_itf->handle(),
                                            m_acl->handle()));
                }
            }

            /**
             * The direction the of the packets on which to apply the ACL
             * input or output
             */
            const direction_t m_direction;

            /**
             * A reference counting pointer the interface that this L3 layer
             * represents. By holding the reference here, we can guarantee that
             * this object will outlive the interface
             */
            const std::shared_ptr<interface> m_itf;
    
            /**
             * A reference counting pointer the ACL that this
             * interface is bound to. By holding the reference here, we can
             * guarantee that this object will outlive the BD.
             */
            const std::shared_ptr<LIST> m_acl;

            /**
             * HW configuration for the binding. The bool representing the
             * do/don't bind.
             */
            HW::item<bool> m_binding;

            /**
             * A map of all L2 interfaces key against the interface's handle_t
             */
            static singular_db<key_t, binding> m_db;
        };

        /**
         * Typedef the L3 binding type
         */
        typedef binding<l3_list,
                        vapi::Acl_interface_add_del,
                        vapi::Acl_interface_list_dump> l3_binding;

        /**
         * Typedef the L2 binding type
         */
        typedef binding<l2_list,
                        vapi::Macip_acl_interface_add_del,
                        vapi::Macip_acl_interface_list_dump> l2_binding;

        /**
         * Definition of the static Singular DB for ACL bindings
         */
        template <typename LIST, typename BIND, typename DUMP>
        singular_db<typename ACL::binding<LIST, BIND, DUMP>::key_t,
                   ACL::binding<LIST, BIND, DUMP>> binding<LIST, BIND, DUMP>::m_db;
        
        template <typename LIST, typename BIND, typename DUMP>
        typename ACL::binding<LIST, BIND, DUMP>::event_handler binding<LIST, BIND, DUMP>::m_evh;
    };

    std::ostream &operator<<(std::ostream &os,
                             const std::pair<direction_t,
                                             interface::key_type> &key);
};

#endif
