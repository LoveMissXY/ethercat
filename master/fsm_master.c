/******************************************************************************
 *
 *  $Id$
 *
 *  Copyright (C) 2006-2008  Florian Pose, Ingenieurgemeinschaft IgH
 *
 *  This file is part of the IgH EtherCAT Master.
 *
 *  The IgH EtherCAT Master is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU General Public License version 2, as
 *  published by the Free Software Foundation.
 *
 *  The IgH EtherCAT Master is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General
 *  Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along
 *  with the IgH EtherCAT Master; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 *  ---
 *
 *  The license mentioned above concerns the source code only. Using the
 *  EtherCAT technology and brand is only permitted in compliance with the
 *  industrial property and similar rights of Beckhoff Automation GmbH.
 *
 *****************************************************************************/

/** \file
 * EtherCAT master state machine.
 */

/*****************************************************************************/

#include "globals.h"
#include "master.h"
#include "mailbox.h"
#include "slave_config.h"
#ifdef EC_EOE
#include "ethernet.h"
#endif

#include "fsm_master.h"
#include "fsm_foe.h"

/*****************************************************************************/

/** Time difference [ns] to tolerate without setting a new system time offset.
 */
#ifdef EC_HAVE_CYCLES
#define EC_SYSTEM_TIME_TOLERANCE_NS 10000
#else
#define EC_SYSTEM_TIME_TOLERANCE_NS 100000000
#endif

/*****************************************************************************/

void ec_fsm_master_state_start(ec_fsm_master_t *);
void ec_fsm_master_state_broadcast(ec_fsm_master_t *);
void ec_fsm_master_state_read_state(ec_fsm_master_t *);
void ec_fsm_master_state_acknowledge(ec_fsm_master_t *);
void ec_fsm_master_state_configure_slave(ec_fsm_master_t *);
void ec_fsm_master_state_clear_addresses(ec_fsm_master_t *);
void ec_fsm_master_state_dc_measure_delays(ec_fsm_master_t *);
void ec_fsm_master_state_scan_slave(ec_fsm_master_t *);
void ec_fsm_master_state_dc_read_offset(ec_fsm_master_t *);
void ec_fsm_master_state_dc_write_offset(ec_fsm_master_t *);
void ec_fsm_master_state_write_sii(ec_fsm_master_t *);
void ec_fsm_master_state_sdo_dictionary(ec_fsm_master_t *);
void ec_fsm_master_state_sdo_request(ec_fsm_master_t *);
void ec_fsm_master_state_reg_request(ec_fsm_master_t *);

void ec_fsm_master_enter_write_system_times(ec_fsm_master_t *);

/*****************************************************************************/

/** Constructor.
 */
void ec_fsm_master_init(
        ec_fsm_master_t *fsm, /**< Master state machine. */
        ec_master_t *master, /**< EtherCAT master. */
        ec_datagram_t *datagram /**< Datagram object to use. */
        )
{
    fsm->master = master;
    fsm->datagram = datagram;
    fsm->state = ec_fsm_master_state_start;
    fsm->idle = 0;
    fsm->link_state = 0;
    fsm->slaves_responding = 0;
    fsm->rescan_required = 0;
    fsm->slave_states = EC_SLAVE_STATE_UNKNOWN;

    // init sub-state-machines
    ec_fsm_coe_init(&fsm->fsm_coe, fsm->datagram);
    ec_fsm_pdo_init(&fsm->fsm_pdo, &fsm->fsm_coe);
    ec_fsm_change_init(&fsm->fsm_change, fsm->datagram);
    ec_fsm_slave_config_init(&fsm->fsm_slave_config, fsm->datagram,
            &fsm->fsm_change, &fsm->fsm_coe, &fsm->fsm_pdo);
    ec_fsm_slave_scan_init(&fsm->fsm_slave_scan, fsm->datagram,
            &fsm->fsm_slave_config, &fsm->fsm_pdo);
    ec_fsm_sii_init(&fsm->fsm_sii, fsm->datagram);
}

/*****************************************************************************/

/** Destructor.
 */
void ec_fsm_master_clear(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    // clear sub-state machines
    ec_fsm_coe_clear(&fsm->fsm_coe);
    ec_fsm_pdo_clear(&fsm->fsm_pdo);
    ec_fsm_change_clear(&fsm->fsm_change);
    ec_fsm_slave_config_clear(&fsm->fsm_slave_config);
    ec_fsm_slave_scan_clear(&fsm->fsm_slave_scan);
    ec_fsm_sii_clear(&fsm->fsm_sii);
}

/*****************************************************************************/

/** Executes the current state of the state machine.
 *
 * If the state machine's datagram is not sent or received yet, the execution
 * of the state machine is delayed to the next cycle.
 *
 * \return true, if the state machine was executed
 */
int ec_fsm_master_exec(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    if (fsm->datagram->state == EC_DATAGRAM_SENT
        || fsm->datagram->state == EC_DATAGRAM_QUEUED) {
        // datagram was not sent or received yet.
        return 0;
    }

    fsm->state(fsm);
    return 1;
}

/*****************************************************************************/

/**
 * \return true, if the state machine is in an idle phase
 */
int ec_fsm_master_idle(
        const ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    return fsm->idle;
}

/*****************************************************************************/

/** Restarts the master state machine.
 */
void ec_fsm_master_restart(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    fsm->state = ec_fsm_master_state_start;
    fsm->state(fsm); // execute immediately
}

/******************************************************************************
 * Master state machine
 *****************************************************************************/

/** Master state: START.
 *
 * Starts with getting slave count and slave states.
 */
void ec_fsm_master_state_start(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    fsm->idle = 1;
    ec_datagram_brd(fsm->datagram, 0x0130, 2);
    ec_datagram_zero(fsm->datagram);
    fsm->state = ec_fsm_master_state_broadcast;
}

/*****************************************************************************/

/** Master state: BROADCAST.
 *
 * Processes the broadcast read slave count and slaves states.
 */
void ec_fsm_master_state_broadcast(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_datagram_t *datagram = fsm->datagram;
    unsigned int i, size;
    ec_slave_t *slave;
    ec_master_t *master = fsm->master;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT)
        return; // always retry

    // bus topology change?
    if (datagram->working_counter != fsm->slaves_responding) {
        fsm->rescan_required = 1;
        fsm->slaves_responding = datagram->working_counter;
        EC_MASTER_INFO(master, "%u slave(s) responding.\n",
                fsm->slaves_responding);
    }

    if (fsm->link_state && !master->main_device.link_state) { // link went down
        EC_MASTER_DBG(master, 1, "Master state machine detected "
                "link down. Clearing slave list.\n");

#ifdef EC_EOE
        ec_master_eoe_stop(master);
        ec_master_clear_eoe_handlers(master);
#endif
        ec_master_clear_slaves(master);
        fsm->slave_states = 0x00;
    }
    fsm->link_state = master->main_device.link_state;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        ec_fsm_master_restart(fsm);
        return;
    }

    if (fsm->slaves_responding) {
        uint8_t states = EC_READ_U8(datagram->data);
        if (states != fsm->slave_states) { // slave states changed?
            char state_str[EC_STATE_STRING_SIZE];
            fsm->slave_states = states;
            ec_state_string(fsm->slave_states, state_str, 1);
            EC_MASTER_INFO(master, "Slave states: %s.\n", state_str);
        }
    } else {
        fsm->slave_states = 0x00;
    }

    if (fsm->rescan_required) {
        down(&master->scan_sem);
        if (!master->allow_scan) {
            up(&master->scan_sem);
        } else {
            master->scan_busy = 1;
            up(&master->scan_sem);

            // clear all slaves and scan the bus
            fsm->rescan_required = 0;
            fsm->idle = 0;
            fsm->scan_jiffies = jiffies;

#ifdef EC_EOE
            ec_master_eoe_stop(master);
            ec_master_clear_eoe_handlers(master);
#endif
            ec_master_clear_slaves(master);

            master->slave_count = fsm->slaves_responding;

            if (!master->slave_count) {
                // no slaves present -> finish state machine.
                master->scan_busy = 0;
                wake_up_interruptible(&master->scan_queue);
                ec_fsm_master_restart(fsm);
                return;
            }

            size = sizeof(ec_slave_t) * master->slave_count;
            if (!(master->slaves = (ec_slave_t *) kmalloc(size, GFP_KERNEL))) {
                EC_MASTER_ERR(master, "Failed to allocate %u bytes"
                        " of slave memory!\n", size);
                master->slave_count = 0; // TODO avoid retrying scan!
                master->scan_busy = 0;
                wake_up_interruptible(&master->scan_queue);
                ec_fsm_master_restart(fsm);
                return;
            }

            // init slaves
            for (i = 0; i < master->slave_count; i++) {
                slave = master->slaves + i;
                ec_slave_init(slave, master, i, i + 1);

                // do not force reconfiguration in operation phase to avoid
                // unnecesssary process data interruptions
                if (master->phase != EC_OPERATION)
                    slave->force_config = 1;
            }

            // broadcast clear all station addresses
            ec_datagram_bwr(datagram, 0x0010, 2);
            EC_WRITE_U16(datagram->data, 0x0000);
            fsm->retries = EC_FSM_RETRIES;
            fsm->state = ec_fsm_master_state_clear_addresses;
            return;
        }
    }

    if (master->slave_count) {

        // application applied configurations
        if (master->config_changed) {
            master->config_changed = 0;

            EC_MASTER_DBG(master, 1, "Configuration changed.\n");

            fsm->slave = master->slaves; // begin with first slave
            ec_fsm_master_enter_write_system_times(fsm);

        } else {
            // fetch state from first slave
            fsm->slave = master->slaves;
            ec_datagram_fprd(fsm->datagram, fsm->slave->station_address,
                    0x0130, 2);
            ec_datagram_zero(datagram);
            fsm->retries = EC_FSM_RETRIES;
            fsm->state = ec_fsm_master_state_read_state;
        }
    } else {
        ec_fsm_master_restart(fsm);
    }
}

/*****************************************************************************/

/** Check for pending SII write requests and process one.
 *
 * \return non-zero, if an SII write request is processed.
 */
int ec_fsm_master_action_process_sii(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_sii_write_request_t *request;

    // search the first request to be processed
    while (1) {
        if (list_empty(&master->sii_requests))
            break;

        // get first request
        request = list_entry(master->sii_requests.next,
                ec_sii_write_request_t, list);
        list_del_init(&request->list); // dequeue
        request->state = EC_INT_REQUEST_BUSY;

        // found pending SII write operation. execute it!
        EC_SLAVE_DBG(request->slave, 1, "Writing SII data...\n");
        fsm->sii_request = request;
        fsm->sii_index = 0;
        ec_fsm_sii_write(&fsm->fsm_sii, request->slave, request->offset,
                request->words, EC_FSM_SII_USE_CONFIGURED_ADDRESS);
        fsm->state = ec_fsm_master_state_write_sii;
        fsm->state(fsm); // execute immediately
        return 1;
    }

    return 0;
}

/*****************************************************************************/

/** Check for pending register requests and process one.
 *
 * \return non-zero, if a register request is processed.
 */
int ec_fsm_master_action_process_register(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_reg_request_t *request;

    // search the first request to be processed
    while (!list_empty(&master->reg_requests)) {

        // get first request
        request = list_entry(master->reg_requests.next,
                ec_reg_request_t, list);
        list_del_init(&request->list); // dequeue
        request->state = EC_INT_REQUEST_BUSY;

        // found pending request; process it!
        EC_SLAVE_DBG(request->slave, 1, "Processing register request, "
                "offset 0x%04x, length %zu...\n",
                request->offset, request->length);

        if (request->length > fsm->datagram->mem_size) {
            EC_MASTER_ERR(master, "Request length (%zu) exceeds maximum "
                    "datagram size (%zu)!\n", request->length,
                    fsm->datagram->mem_size);
            request->state = EC_INT_REQUEST_FAILURE;
            wake_up(&master->reg_queue);
            continue;
        }

        fsm->reg_request = request;

        if (request->dir == EC_DIR_INPUT) {
            ec_datagram_fprd(fsm->datagram, request->slave->station_address,
                    request->offset, request->length);
            ec_datagram_zero(fsm->datagram);
        } else {
            ec_datagram_fpwr(fsm->datagram, request->slave->station_address,
                    request->offset, request->length);
            memcpy(fsm->datagram->data, request->data, request->length);
        }
        fsm->retries = EC_FSM_RETRIES;
        fsm->state = ec_fsm_master_state_reg_request;
        return 1;
    }

    return 0;
}

/*****************************************************************************/

/** Check for pending SDO requests and process one.
 *
 * \return non-zero, if an SDO request is processed.
 */
int ec_fsm_master_action_process_sdo(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_slave_t *slave;
    ec_sdo_request_t *req;

    // search for internal requests to be processed
    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {
        if (!slave->config)
            continue;
        list_for_each_entry(req, &slave->config->sdo_requests, list) {
            if (req->state == EC_INT_REQUEST_QUEUED) {

                if (ec_sdo_request_timed_out(req)) {
                    req->state = EC_INT_REQUEST_FAILURE;
                    EC_SLAVE_DBG(slave, 1, "Internal SDO request"
                            " timed out.\n");
                    continue;
                }

                if (slave->current_state == EC_SLAVE_STATE_INIT) {
                    req->state = EC_INT_REQUEST_FAILURE;
                    continue;
                }

                req->state = EC_INT_REQUEST_BUSY;
                EC_SLAVE_DBG(slave, 1, "Processing internal"
                        " SDO request...\n");
                fsm->idle = 0;
                fsm->sdo_request = req;
                fsm->slave = slave;
                fsm->state = ec_fsm_master_state_sdo_request;
                ec_fsm_coe_transfer(&fsm->fsm_coe, slave, req);
                ec_fsm_coe_exec(&fsm->fsm_coe); // execute immediately
                return 1;
            }
        }
    }
    return 0;
}


/*****************************************************************************/

/** Master action: IDLE.
 *
 * Does secondary work.
 */
void ec_fsm_master_action_idle(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_slave_t *slave;

    // Check for pending internal SDO requests
    if (ec_fsm_master_action_process_sdo(fsm))
        return;

    // enable processing of requests
    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {
        ec_fsm_slave_ready(&slave->fsm);
    }

    // check, if slaves have an SDO dictionary to read out.
    for (slave = master->slaves;
            slave < master->slaves + master->slave_count;
            slave++) {
        if (!(slave->sii.mailbox_protocols & EC_MBOX_COE)
                || (slave->sii.has_general
                    && !slave->sii.coe_details.enable_sdo_info)
                || slave->sdo_dictionary_fetched
                || slave->current_state == EC_SLAVE_STATE_INIT
                || slave->current_state == EC_SLAVE_STATE_UNKNOWN
                || jiffies - slave->jiffies_preop < EC_WAIT_SDO_DICT * HZ
                ) continue;

        EC_SLAVE_DBG(slave, 1, "Fetching SDO dictionary.\n");

        slave->sdo_dictionary_fetched = 1;

        // start fetching SDO dictionary
        fsm->idle = 0;
        fsm->slave = slave;
        fsm->state = ec_fsm_master_state_sdo_dictionary;
        ec_fsm_coe_dictionary(&fsm->fsm_coe, slave);
        ec_fsm_coe_exec(&fsm->fsm_coe); // execute immediately
        return;
    }

    // check for pending SII write operations.
    if (ec_fsm_master_action_process_sii(fsm))
        return; // SII write request found

    // check for pending register requests.
    if (ec_fsm_master_action_process_register(fsm))
        return; // register request processing

    ec_fsm_master_restart(fsm);
}

/*****************************************************************************/

/** Master action: Get state of next slave.
 */
void ec_fsm_master_action_next_slave_state(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;

    // is there another slave to query?
    fsm->slave++;
    if (fsm->slave < master->slaves + master->slave_count) {
        // fetch state from next slave
        fsm->idle = 1;
        ec_datagram_fprd(fsm->datagram,
                fsm->slave->station_address, 0x0130, 2);
        ec_datagram_zero(fsm->datagram);
        fsm->retries = EC_FSM_RETRIES;
        fsm->state = ec_fsm_master_state_read_state;
        return;
    }

    // all slaves processed
    ec_fsm_master_action_idle(fsm);
}

/*****************************************************************************/

/** Master action: Configure.
 */
void ec_fsm_master_action_configure(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_slave_t *slave = fsm->slave;

    // Does the slave have to be configured?
    if ((slave->current_state != slave->requested_state
                || slave->force_config) && !slave->error_flag) {

        // Start slave configuration, if it is allowed.
        down(&master->config_sem);
        if (!master->allow_config) {
            up(&master->config_sem);
        } else {
            master->config_busy = 1;
            up(&master->config_sem);

            if (master->debug_level) {
                char old_state[EC_STATE_STRING_SIZE],
                     new_state[EC_STATE_STRING_SIZE];
                ec_state_string(slave->current_state, old_state, 0);
                ec_state_string(slave->requested_state, new_state, 0);
                EC_SLAVE_DBG(slave, 1, "Changing state from %s to %s%s.\n",
                        old_state, new_state,
                        slave->force_config ? " (forced)" : "");
            }

            fsm->idle = 0;
            fsm->state = ec_fsm_master_state_configure_slave;
            ec_fsm_slave_config_start(&fsm->fsm_slave_config, slave);
            fsm->state(fsm); // execute immediately
            return;
        }
    }

    // slave has error flag set; process next one
    ec_fsm_master_action_next_slave_state(fsm);
}

/*****************************************************************************/

/** Master state: READ STATE.
 *
 * Fetches the AL state of a slave.
 */
void ec_fsm_master_state_read_state(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_slave_t *slave = fsm->slave;
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_SLAVE_ERR(slave, "Failed to receive AL state datagram: ");
        ec_datagram_print_state(datagram);
        ec_fsm_master_restart(fsm);
        return;
    }

    // did the slave not respond to its station address?
    if (datagram->working_counter != 1) {
        if (!slave->error_flag) {
            slave->error_flag = 1;
            EC_SLAVE_DBG(slave, 1, "Slave did not respond to state query.\n");
        }
        fsm->rescan_required = 1;
        ec_fsm_master_restart(fsm);
        return;
    }

    // A single slave responded
    ec_slave_set_state(slave, EC_READ_U8(datagram->data));

    if (!slave->error_flag) {
        // Check, if new slave state has to be acknowledged
        if (slave->current_state & EC_SLAVE_STATE_ACK_ERR) {
            fsm->idle = 0;
            fsm->state = ec_fsm_master_state_acknowledge;
            ec_fsm_change_ack(&fsm->fsm_change, slave);
            fsm->state(fsm); // execute immediately
            return;
        }

        // No acknowlegde necessary; check for configuration
        ec_fsm_master_action_configure(fsm);
        return;
    }

    // slave has error flag set; process next one
    ec_fsm_master_action_next_slave_state(fsm);
}

/*****************************************************************************/

/** Master state: ACKNOWLEDGE.
 */
void ec_fsm_master_state_acknowledge(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_slave_t *slave = fsm->slave;

    if (ec_fsm_change_exec(&fsm->fsm_change))
        return;

    if (!ec_fsm_change_success(&fsm->fsm_change)) {
        fsm->slave->error_flag = 1;
        EC_SLAVE_ERR(slave, "Failed to acknowledge state change.\n");
    }

    ec_fsm_master_action_configure(fsm);
}

/*****************************************************************************/

/** Master state: CLEAR ADDRESSES.
 */
void ec_fsm_master_state_clear_addresses(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_MASTER_ERR(master, "Failed to receive address"
                " clearing datagram: ");
        ec_datagram_print_state(datagram);
        master->scan_busy = 0;
        wake_up_interruptible(&master->scan_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    if (datagram->working_counter != master->slave_count) {
        EC_MASTER_WARN(master, "Failed to clear all station addresses:"
                " Cleared %u of %u",
                datagram->working_counter, master->slave_count);
    }

    EC_MASTER_DBG(master, 1, "Sending broadcast-write"
            " to measure transmission delays.\n");

    ec_datagram_bwr(datagram, 0x0900, 1);
    ec_datagram_zero(datagram);
    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_master_state_dc_measure_delays;
}

/*****************************************************************************/

/** Master state: DC MEASURE DELAYS.
 */
void ec_fsm_master_state_dc_measure_delays(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_datagram_t *datagram = fsm->datagram;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_MASTER_ERR(master, "Failed to receive delay measuring datagram: ");
        ec_datagram_print_state(datagram);
        master->scan_busy = 0;
        wake_up_interruptible(&master->scan_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    EC_MASTER_DBG(master, 1, "%u slaves responded to delay measuring.\n",
            datagram->working_counter);

    EC_MASTER_INFO(master, "Scanning bus.\n");

    // begin scanning of slaves
    fsm->slave = master->slaves;
    fsm->state = ec_fsm_master_state_scan_slave;
    ec_fsm_slave_scan_start(&fsm->fsm_slave_scan, fsm->slave);
    ec_fsm_slave_scan_exec(&fsm->fsm_slave_scan); // execute immediately
}

/*****************************************************************************/

/** Master state: SCAN SLAVE.
 *
 * Executes the sub-statemachine for the scanning of a slave.
 */
void ec_fsm_master_state_scan_slave(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
#ifdef EC_EOE
    ec_slave_t *slave = fsm->slave;
#endif
    if (ec_fsm_slave_scan_exec(&fsm->fsm_slave_scan))
        return;

#ifdef EC_EOE
    if (slave->sii.mailbox_protocols & EC_MBOX_EOE) {
        // create EoE handler for this slave
        ec_eoe_t *eoe;
        if (!(eoe = kmalloc(sizeof(ec_eoe_t), GFP_KERNEL))) {
            EC_SLAVE_ERR(slave, "Failed to allocate EoE handler memory!\n");
        } else if (ec_eoe_init(eoe, slave)) {
            EC_SLAVE_ERR(slave, "Failed to init EoE handler!\n");
            kfree(eoe);
        } else {
            list_add_tail(&eoe->list, &master->eoe_handlers);
        }
    }
#endif

    // another slave to fetch?
    fsm->slave++;
    if (fsm->slave < master->slaves + master->slave_count) {
        ec_fsm_slave_scan_start(&fsm->fsm_slave_scan, fsm->slave);
        ec_fsm_slave_scan_exec(&fsm->fsm_slave_scan); // execute immediately
        return;
    }

    EC_MASTER_INFO(master, "Bus scanning completed in %lu ms.\n",
            (jiffies - fsm->scan_jiffies) * 1000 / HZ);

    master->scan_busy = 0;
    wake_up_interruptible(&master->scan_queue);

    ec_master_calc_dc(master);

    // Attach slave configurations
    ec_master_attach_slave_configs(master);

#ifdef EC_EOE
    // check if EoE processing has to be started
    ec_master_eoe_start(master);
#endif

    if (master->slave_count) {
        fsm->slave = master->slaves; // begin with first slave
        ec_fsm_master_enter_write_system_times(fsm);
    } else {
        ec_fsm_master_restart(fsm);
    }
}

/*****************************************************************************/

/** Master state: CONFIGURE SLAVE.
 *
 * Starts configuring a slave.
 */
void ec_fsm_master_state_configure_slave(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;

    if (ec_fsm_slave_config_exec(&fsm->fsm_slave_config))
        return;

    fsm->slave->force_config = 0;

    // configuration finished
    master->config_busy = 0;
    wake_up_interruptible(&master->config_queue);

    if (!ec_fsm_slave_config_success(&fsm->fsm_slave_config)) {
        // TODO: mark slave_config as failed.
    }

    fsm->idle = 1;
    ec_fsm_master_action_next_slave_state(fsm);
}

/*****************************************************************************/

/** Start writing DC system times.
 */
void ec_fsm_master_enter_write_system_times(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;

    EC_MASTER_DBG(master, 1, "Writing system time offsets...\n");

    if (master->has_app_time) {
        while (fsm->slave < master->slaves + master->slave_count) {
            if (!fsm->slave->base_dc_supported
                    || !fsm->slave->has_dc_system_time) {
                fsm->slave++;
                continue;
            }

            // read DC system time (0x0910, 64 bit)
            //                         gap (64 bit)
            //     and time offset (0x0920, 64 bit)
            ec_datagram_fprd(fsm->datagram, fsm->slave->station_address,
                    0x0910, 24);
            fsm->retries = EC_FSM_RETRIES;
            fsm->state = ec_fsm_master_state_dc_read_offset;
            return;
        }
    } else {
        EC_MASTER_DBG(master, 1, "No app_time received up to now.\n");
    }

    ec_master_request_op(master);
    ec_fsm_master_restart(fsm);
}

/*****************************************************************************/

/** Configure 32 bit time offset.
 */
u64 ec_fsm_master_dc_offset32(
        ec_fsm_master_t *fsm, /**< Master state machine. */
        u64 system_time, /**< System time register. */
        u64 old_offset, /**< Time offset register. */
		u64 correction /**< Correction. */
        )
{
    ec_slave_t *slave = fsm->slave;
	u32 correction32, system_time32, old_offset32, new_offset;
    s32 time_diff;

	system_time32 = (u32) system_time;
	// correct read system time by elapsed time between read operation
	// and app_time set time
	correction32 = (u32)correction;
	system_time32 -= correction32;
	old_offset32 = (u32) old_offset;

    time_diff = (u32) slave->master->app_time - system_time32;

    EC_SLAVE_DBG(slave, 1, "DC system time offset calculation:"
            " system_time=%u (corrected with %u),"
            " app_time=%llu, diff=%i\n",
			system_time32, correction32,
            slave->master->app_time, time_diff);

    if (EC_ABS(time_diff) > EC_SYSTEM_TIME_TOLERANCE_NS) {
        new_offset = time_diff + old_offset32;
        EC_SLAVE_DBG(slave, 1, "Setting time offset to %u (was %u)\n",
                new_offset, old_offset32);
        return (u64) new_offset;
    } else {
        EC_SLAVE_DBG(slave, 1, "Not touching time offset.\n");
        return old_offset;
    }
}

/*****************************************************************************/

/** Configure 64 bit time offset.
 */
u64 ec_fsm_master_dc_offset64(
        ec_fsm_master_t *fsm, /**< Master state machine. */
        u64 system_time, /**< System time register. */
        u64 old_offset, /**< Time offset register. */
		u64 correction /**< Correction. */
        )
{
    ec_slave_t *slave = fsm->slave;
	u64 new_offset;
    s64 time_diff;

	// correct read system time by elapsed time between read operation
	// and app_time set time
	system_time -= correction;
	time_diff = fsm->slave->master->app_time - system_time;

    EC_SLAVE_DBG(slave, 1, "DC system time offset calculation:"
            " system_time=%llu (corrected with %llu),"
            " app_time=%llu, diff=%lli\n",
            system_time, correction,
            slave->master->app_time, time_diff);

    if (EC_ABS(time_diff) > EC_SYSTEM_TIME_TOLERANCE_NS) {
        new_offset = time_diff + old_offset;
        EC_SLAVE_DBG(slave, 1, "Setting time offset to %llu (was %llu)\n",
                new_offset, old_offset);
    } else {
        new_offset = old_offset;
        EC_SLAVE_DBG(slave, 1, "Not touching time offset.\n");
    }

    return new_offset;
}

/*****************************************************************************/

/** Master state: DC READ OFFSET.
 */
void ec_fsm_master_state_dc_read_offset(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_datagram_t *datagram = fsm->datagram;
    ec_slave_t *slave = fsm->slave;
	u64 system_time, old_offset, new_offset, correction;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_SLAVE_ERR(slave, "Failed to receive DC times datagram: ");
        ec_datagram_print_state(datagram);
        fsm->slave++;
        ec_fsm_master_enter_write_system_times(fsm);
        return;
    }

    if (datagram->working_counter != 1) {
        EC_SLAVE_WARN(slave, "Failed to get DC times: ");
        ec_datagram_print_wc_error(datagram);
        fsm->slave++;
        ec_fsm_master_enter_write_system_times(fsm);
        return;
    }

    system_time = EC_READ_U64(datagram->data);     // 0x0910
    old_offset = EC_READ_U64(datagram->data + 16); // 0x0920
	/* correct read system time by elapsed time since read operation
	   and the app_time set time */
#ifdef EC_HAVE_CYCLES
	correction =
			(datagram->cycles_sent - slave->master->dc_cycles_app_time)
			* 1000000LL;
	do_div(correction,cpu_khz);
#else
	correction =
			(u64) ((datagram->jiffies_sent-slave->master->dc_jiffies_app_time) * 1000 / HZ)
			* 1000000;
#endif

    if (slave->base_dc_range == EC_DC_32) {
        new_offset = ec_fsm_master_dc_offset32(fsm,
				system_time, old_offset, correction);
    } else {
        new_offset = ec_fsm_master_dc_offset64(fsm,
				system_time, old_offset, correction);
    }

    // set DC system time offset and transmission delay
    ec_datagram_fpwr(datagram, slave->station_address, 0x0920, 12);
    EC_WRITE_U64(datagram->data, new_offset);
    EC_WRITE_U32(datagram->data + 8, slave->transmission_delay);
    fsm->retries = EC_FSM_RETRIES;
    fsm->state = ec_fsm_master_state_dc_write_offset;
}

/*****************************************************************************/

/** Master state: DC WRITE OFFSET.
 */
void ec_fsm_master_state_dc_write_offset(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_datagram_t *datagram = fsm->datagram;
    ec_slave_t *slave = fsm->slave;

    if (datagram->state == EC_DATAGRAM_TIMED_OUT && fsm->retries--)
        return;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_SLAVE_ERR(slave,
                "Failed to receive DC system time offset datagram: ");
        ec_datagram_print_state(datagram);
        fsm->slave++;
        ec_fsm_master_enter_write_system_times(fsm);
        return;
    }

    if (datagram->working_counter != 1) {
        EC_SLAVE_ERR(slave, "Failed to set DC system time offset: ");
        ec_datagram_print_wc_error(datagram);
        fsm->slave++;
        ec_fsm_master_enter_write_system_times(fsm);
        return;
    }

    fsm->slave++;
    ec_fsm_master_enter_write_system_times(fsm);
}

/*****************************************************************************/

/** Master state: WRITE SII.
 */
void ec_fsm_master_state_write_sii(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_sii_write_request_t *request = fsm->sii_request;
    ec_slave_t *slave = request->slave;

    if (ec_fsm_sii_exec(&fsm->fsm_sii)) return;

    if (!ec_fsm_sii_success(&fsm->fsm_sii)) {
        EC_SLAVE_ERR(slave, "Failed to write SII data.\n");
        request->state = EC_INT_REQUEST_FAILURE;
        wake_up(&master->sii_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    fsm->sii_index++;
    if (fsm->sii_index < request->nwords) {
        ec_fsm_sii_write(&fsm->fsm_sii, slave,
                request->offset + fsm->sii_index,
                request->words + fsm->sii_index,
                EC_FSM_SII_USE_CONFIGURED_ADDRESS);
        ec_fsm_sii_exec(&fsm->fsm_sii); // execute immediately
        return;
    }

    // finished writing SII
    EC_SLAVE_DBG(slave, 1, "Finished writing %zu words of SII data.\n",
            request->nwords);

    if (request->offset <= 4 && request->offset + request->nwords > 4) {
        // alias was written
        slave->sii.alias = EC_READ_U16(request->words + 4);
        // TODO: read alias from register 0x0012
        slave->effective_alias = slave->sii.alias;
    }
    // TODO: Evaluate other SII contents!

    request->state = EC_INT_REQUEST_SUCCESS;
    wake_up(&master->sii_queue);

    // check for another SII write request
    if (ec_fsm_master_action_process_sii(fsm))
        return; // processing another request

    ec_fsm_master_restart(fsm);
}

/*****************************************************************************/

/** Master state: SDO DICTIONARY.
 */
void ec_fsm_master_state_sdo_dictionary(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_slave_t *slave = fsm->slave;
    ec_master_t *master = fsm->master;

    if (ec_fsm_coe_exec(&fsm->fsm_coe)) return;

    if (!ec_fsm_coe_success(&fsm->fsm_coe)) {
        ec_fsm_master_restart(fsm);
        return;
    }

    // SDO dictionary fetching finished

    if (master->debug_level) {
        unsigned int sdo_count, entry_count;
        ec_slave_sdo_dict_info(slave, &sdo_count, &entry_count);
        EC_SLAVE_DBG(slave, 1, "Fetched %u SDOs and %u entries.\n",
               sdo_count, entry_count);
    }

    // attach pdo names from dictionary
    ec_slave_attach_pdo_names(slave);

    ec_fsm_master_restart(fsm);
}

/*****************************************************************************/

/** Master state: SDO REQUEST.
 */
void ec_fsm_master_state_sdo_request(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_sdo_request_t *request = fsm->sdo_request;

    if (ec_fsm_coe_exec(&fsm->fsm_coe)) return;

    if (!ec_fsm_coe_success(&fsm->fsm_coe)) {
        EC_SLAVE_DBG(fsm->slave, 1,
                "Failed to process internal SDO request.\n");
        request->state = EC_INT_REQUEST_FAILURE;
        wake_up(&fsm->slave->sdo_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    // SDO request finished
    request->state = EC_INT_REQUEST_SUCCESS;
    wake_up(&fsm->slave->sdo_queue);

    EC_SLAVE_DBG(fsm->slave, 1, "Finished internal SDO request.\n");

    // check for another SDO request
    if (ec_fsm_master_action_process_sdo(fsm))
        return; // processing another request

    ec_fsm_master_restart(fsm);
}

/*****************************************************************************/

/** Master state: REG REQUEST.
 */
void ec_fsm_master_state_reg_request(
        ec_fsm_master_t *fsm /**< Master state machine. */
        )
{
    ec_master_t *master = fsm->master;
    ec_datagram_t *datagram = fsm->datagram;
    ec_reg_request_t *request = fsm->reg_request;

    if (datagram->state != EC_DATAGRAM_RECEIVED) {
        EC_MASTER_ERR(master, "Failed to receive register"
                " request datagram: ");
        ec_datagram_print_state(datagram);
        request->state = EC_INT_REQUEST_FAILURE;
        wake_up(&master->reg_queue);
        ec_fsm_master_restart(fsm);
        return;
    }

    if (datagram->working_counter == 1) {
        if (request->dir == EC_DIR_INPUT) { // read request
            if (request->data)
                kfree(request->data);
            request->data = kmalloc(request->length, GFP_KERNEL);
            if (!request->data) {
                EC_MASTER_ERR(master, "Failed to allocate %zu bytes"
                        " of memory for register data.\n", request->length);
                request->state = EC_INT_REQUEST_FAILURE;
                wake_up(&master->reg_queue);
                ec_fsm_master_restart(fsm);
                return;
            }
            memcpy(request->data, datagram->data, request->length);
        }

        request->state = EC_INT_REQUEST_SUCCESS;
        EC_SLAVE_DBG(request->slave, 1, "Register request successful.\n");
    } else {
        request->state = EC_INT_REQUEST_FAILURE;
        EC_MASTER_ERR(master, "Register request failed.\n");
    }

    wake_up(&master->reg_queue);

    // check for another register request
    if (ec_fsm_master_action_process_register(fsm))
        return; // processing another request

    ec_fsm_master_restart(fsm);
}

/*****************************************************************************/
