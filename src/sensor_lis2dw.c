// Support for gathering acceleration data from LIS2DW chip
//
// Copyright (C) 2023  Zhou.XianMing <zhouxm@biqu3d.com>
// Copyright (C) 2020-2023  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <string.h> // memcpy
#include "board/irq.h" // irq_disable
#include "board/misc.h" // timer_read_time
#include "basecmd.h" // oid_alloc
#include "command.h" // DECL_COMMAND
#include "sched.h" // DECL_TASK
#include "sensor_bulk.h" // sensor_bulk_report
#include "spicmds.h" // spidev_transfer
#include "i2ccmds.h" // i2cdev_s

#define LIS_AR_DATAX0 0x28
#define LIS_AM_READ   0x80
#define LIS_MS_SPI    0x40
#define LIS_MS_I2C    0x80
#define LIS_AD_I2C    0x30
#define LIS_FIFO_SAMPLES 0x2F

#define BYTES_PER_SAMPLE 6

struct lis2dw {
    struct timer timer;
    uint32_t rest_ticks;
    void *serial;
    uint8_t serial_type;
    uint8_t flags;
    uint8_t model;
    struct sensor_bulk sb;
};

enum {
    LIS_PENDING = 1<<0,
};

enum {
    SPI_SERIAL = 0,
    I2C_SERIAL = 1,
};

enum {
    LIS2DW = 0,
    LIS3DH = 1,
};

static struct task_wake lis2dw_wake;

// Event handler that wakes lis2dw_task() periodically
static uint_fast8_t
lis2dw_event(struct timer *timer)
{
    struct lis2dw *ax = container_of(timer, struct lis2dw, timer);
    ax->flags |= LIS_PENDING;
    sched_wake_task(&lis2dw_wake);
    return SF_DONE;
}

void
command_config_lis2dw(uint32_t *args)
{
    struct lis2dw *ax = oid_alloc(args[0], command_config_lis2dw
                                   , sizeof(*ax));
    ax->timer.func = lis2dw_event;

    switch (args[2]) {
        case SPI_SERIAL:
            ax->serial = spidev_oid_lookup(args[1]);
            ax->serial_type = SPI_SERIAL;
            break;
        case I2C_SERIAL:
            ax->serial = i2cdev_oid_lookup(args[1]);
            ax->serial_type = I2C_SERIAL;
            break;
        default:
            shutdown("serial_type invalid");
    }

    switch (args[3]) {
        case LIS2DW:
        case LIS3DH:
            ax->model = args[3];
            break;
        default:
            shutdown("model type invalid");
    }
}
DECL_COMMAND(command_config_lis2dw, "config_lis2dw oid=%c serial_oid=%c serial_type=%c model=%c");

// Helper code to reschedule the lis2dw_event() timer
static void
lis2dw_reschedule_timer(struct lis2dw *ax)
{
    irq_disable();
    ax->timer.waketime = timer_read_time() + ax->rest_ticks;
    sched_add_timer(&ax->timer);
    irq_enable();
}

// Query accelerometer data
static void
lis2dw_query(struct lis2dw *ax, uint8_t oid)
{
    uint8_t fifo_empty = 0;
    uint8_t fifo_ovrn = 0;
    uint8_t *d = &ax->sb.data[ax->sb.data_count];

    if (ax->serial_type == SPI_SERIAL) {
        uint8_t msg[7] = {0};
        uint8_t fifo[2] = {LIS_FIFO_SAMPLES | LIS_AM_READ , 0};

        msg[0] = LIS_AR_DATAX0 | LIS_AM_READ;
        if (ax->model == LIS3DH)
            msg[0] |= LIS_MS_SPI;

        spidev_transfer(ax->serial, 1, sizeof(msg), msg);

        spidev_transfer(ax->serial, 1, sizeof(fifo), fifo);

        if (ax->model == LIS3DH)
            fifo_empty = fifo[1] & 0x20;
        else
            fifo_empty = fifo[1] & 0x3F;

        fifo_ovrn = fifo[1] & 0x40;

        for (uint32_t i = 0; i < BYTES_PER_SAMPLE; i++)
            d[i] = msg[i + 1];
    } else {
        uint8_t msg_reg[] = {LIS_AR_DATAX0};
        if (ax->model == LIS3DH)
            msg_reg[0] |= LIS_MS_I2C; 
        uint8_t msg[6];
        uint8_t fifo_reg[1] = {LIS_FIFO_SAMPLES};
        uint8_t fifo[1];

        struct i2cdev_s *i2c = ax->serial;

        i2c_read(i2c->i2c_config, sizeof(msg_reg), msg_reg, sizeof(msg), msg);

        i2c_read(i2c->i2c_config, sizeof(fifo_reg), fifo_reg, sizeof(fifo), fifo);

        if (ax->model == LIS3DH)
            fifo_empty = fifo[0] & 0x20;
        else
            fifo_empty = fifo[0] & 0x3F;

        fifo_ovrn = fifo[0] & 0x40;

        for (uint32_t i = 0; i < BYTES_PER_SAMPLE; i++)
            d[i] = msg[i];
    }

    ax->sb.data_count += BYTES_PER_SAMPLE;
    if (ax->sb.data_count + BYTES_PER_SAMPLE > ARRAY_SIZE(ax->sb.data))
        sensor_bulk_report(&ax->sb, oid);

    // Check fifo status
    if (fifo_ovrn)
        ax->sb.possible_overflows++;

    // check if we need to run the task again (more packets in fifo?)
    if (!fifo_empty) {
        // More data in fifo - wake this task again
        sched_wake_task(&lis2dw_wake);
    } else {
        // Sleep until next check time
        ax->flags &= ~LIS_PENDING;
        lis2dw_reschedule_timer(ax);
    }
}

void
command_query_lis2dw(uint32_t *args)
{
    struct lis2dw *ax = oid_lookup(args[0], command_config_lis2dw);

    sched_del_timer(&ax->timer);
    ax->flags = 0;
    if (!args[1])
        // End measurements
        return;

    // Start new measurements query
    ax->rest_ticks = args[1];
    sensor_bulk_reset(&ax->sb);
    lis2dw_reschedule_timer(ax);
}
DECL_COMMAND(command_query_lis2dw, "query_lis2dw oid=%c rest_ticks=%u");

void
command_query_lis2dw_status(uint32_t *args)
{
    struct lis2dw *ax = oid_lookup(args[0], command_config_lis2dw);
    uint32_t time1 = 0;
    uint32_t time2 = 0;
    uint8_t status = 0;
    if (ax->serial_type == SPI_SERIAL) {
        uint8_t msg[2] = { LIS_FIFO_SAMPLES | LIS_AM_READ, 0x00 };
        time1 = timer_read_time();
        spidev_transfer(ax->serial, 1, sizeof(msg), msg);
        time2 = timer_read_time();
        status = msg[1];
    } else {
        uint8_t fifo_reg[1] = {LIS_FIFO_SAMPLES};
        uint8_t fifo[1];

        struct i2cdev_s *i2c = ax->serial;

        time1 = timer_read_time();
        i2c_read(i2c->i2c_config, sizeof(fifo_reg), fifo_reg, sizeof(fifo), fifo);
        time2 = timer_read_time();
        status = fifo[0];
    }
    sensor_bulk_status(&ax->sb, args[0], time1, time2-time1
                       , (status & 0x1f) * BYTES_PER_SAMPLE);
}
DECL_COMMAND(command_query_lis2dw_status, "query_lis2dw_status oid=%c");

void
lis2dw_task(void)
{
    if (!sched_check_wake(&lis2dw_wake))
        return;
    uint8_t oid;
    struct lis2dw *ax;
    foreach_oid(oid, ax, command_config_lis2dw) {
        uint_fast8_t flags = ax->flags;
        if (flags & LIS_PENDING)
            lis2dw_query(ax, oid);
    }
}
DECL_TASK(lis2dw_task);
