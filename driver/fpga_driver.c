#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/init.h>
#include <linux/interrupt.h>
#include <asm/io.h>
#include <linux/delay.h>
#include <linux/time.h>
#include <linux/timer.h>
#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/workqueue.h>
#include <linux/cdev.h>
#include <linux/mutex.h>
#include <linux/device.h>
#include <asm/uaccess.h>
#include <asm/atomic.h>

#define FPGA_CMD_BUF_SIZE 128

static dev_t fpga_cmd_dev;
static struct cdev fpga_cmd_cdev;
static struct class *fpga_cmd_class;
static struct device *fpga_cmd_device;
static char cmd_buffer[FPGA_CMD_BUF_SIZE];
static int cmd_buffer_len = 0;

static DEFINE_MUTEX(cmd_mutex);
static DEFINE_MUTEX(hw_mutex);
static DEFINE_SPINLOCK(buffer_lock);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Audio Player Team");
MODULE_DESCRIPTION("Hybrid Dual AXI Audio Player - 48kHz I2S 24-bit");


/* Virtual address bases */
void __iomem *lwbridgebase;
void __iomem *audiobase;

/* Audio player state */
typedef enum {
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_STOPPED
} audio_state_t;

static audio_state_t current_state = AUDIO_STOPPED;
static int current_track = 0;
static const int total_tracks = 3;

/* WAV file structure */
typedef struct {
    struct file *file;
    char filename[256];
    uint32_t sample_rate;
    uint16_t channels;
    uint16_t bits_per_sample;
    uint32_t data_size;
    uint32_t data_start_offset;
    uint32_t samples_played;
    uint32_t total_samples;
    uint8_t is_valid;
    loff_t current_pos;
} wav_file_t;

static wav_file_t songs[3];
static const char* song_paths[] = {
    "/home/root/media/sd/songs/song1.wav",
    "/home/root/media/sd/songs/song2.wav",
    "/home/root/media/sd/songs/song3.wav"
};

/* Time tracking */
static int current_time_seconds = 0;
static int current_time_minutes = 0;
static struct timer_list audio_timer;
static struct timer_list display_timer;

/* Button definitions */
#define BUTTON_PLAY_PAUSE   0x1
#define BUTTON_NEXT         0x2
#define BUTTON_PREV         0x4

/* LW AXI Master offsets */
#define BUTTONS_BASE_OFFSET     0x8800
#define BUTTONS_DATA_OFFSET     0x0
#define BUTTONS_INTERRUPT_MASK  0x8
#define BUTTONS_EDGE_CAPTURE    0xC
#define SEVEN_SEGMENTS_BASE_OFFSET 0x8810
#define AUDIO_CONFIG_BASE_OFFSET    0x8850

/* AXI Master offsets */
#define AUDIO_FIFOSPACE_REG     0x4
#define AUDIO_LEFTDATA_REG      0x8
#define AUDIO_RIGHTDATA_REG     0xC
#define AUDIO_CONTROL_REG       0x0

/* Audio buffer - single buffer */
#define AUDIO_BUFFER_SIZE 32768
static uint8_t *audio_buffer;
static int buffer_pos = 0;
static int buffer_size = 0;
static int buffer_needs_refill = 0;

/* Workqueue for file and button operations */
static struct workqueue_struct *audio_wq;
static struct work_struct load_work;
static struct work_struct refill_work;
static struct work_struct button_work;
static atomic_t pending_buttons = ATOMIC_INIT(0);
static int next_track_to_load = 0;


/* Debouncing */
static struct timespec last_button_time[3];
#define DEBOUNCE_TIME_MS 200

/* 7-segment patterns */
static const uint8_t seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

/* Function declarations */
void audio_play(void);
void audio_pause(void);
void audio_next_track(void);
void audio_prev_track(void);
void display_time_mmss(void);
void generate_audio_samples_48khz(void);
void audio_timer_callback(unsigned long data);
void display_timer_callback(unsigned long data);
int is_button_debounced(int button_num);
void button_work_handler(struct work_struct *work);
void load_work_handler(struct work_struct *work);
void refill_work_handler(struct work_struct *work);
int parse_wav_header_48khz(wav_file_t* wav);
void init_wm8731_via_lw_axi(void);
void init_audio_ip_via_axi(void);
void reset_audio_completely(void);

static ssize_t fpga_cmd_read(struct file *file, char __user *buf, size_t count, loff_t *ppos) {
    ssize_t ret = 0;
    if (mutex_lock_interruptible(&cmd_mutex))
        return -ERESTARTSYS;

    if (cmd_buffer_len > 0) {
        size_t to_copy = min(count, (size_t)cmd_buffer_len);
        if (copy_to_user(buf, cmd_buffer, to_copy) == 0) {
            ret = to_copy;
            memmove(cmd_buffer, cmd_buffer + to_copy, cmd_buffer_len - to_copy);
            cmd_buffer_len -= to_copy;
        } else {
            ret = -EFAULT;
        }
    }
    mutex_unlock(&cmd_mutex);
    return ret;
}

static struct file_operations fpga_cmd_fops = {
    .owner = THIS_MODULE,
    .read = fpga_cmd_read,
};

static void send_user_command(char cmd)
{
    mutex_lock(&cmd_mutex);
    if (cmd_buffer_len < FPGA_CMD_BUF_SIZE - 1) {
        cmd_buffer[cmd_buffer_len++] = cmd;
    }
    mutex_unlock(&cmd_mutex);
}

/* WM8731 register addresses */
#define WM8731_LEFT_LINE_IN     0x00
#define WM8731_RIGHT_LINE_IN    0x01
#define WM8731_LEFT_HP_OUT      0x02
#define WM8731_RIGHT_HP_OUT     0x03
#define WM8731_ANALOG_PATH      0x04
#define WM8731_DIGITAL_PATH     0x05
#define WM8731_POWER_DOWN       0x06
#define WM8731_DIGITAL_IF       0x07
#define WM8731_SAMPLING_CTRL    0x08
#define WM8731_ACTIVE_CTRL      0x09
#define WM8731_RESET            0x0F

void init_wm8731_via_lw_axi(void)
{
    printk(KERN_INFO "Initializing WM8731 for 48kHz I2S 24-bit\n");

    mutex_lock(&hw_mutex);

    iowrite32((WM8731_RESET << 9) | 0x00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(10);
    iowrite32((WM8731_POWER_DOWN << 9) | 0x00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(5);
    iowrite32((WM8731_LEFT_HP_OUT << 9) | 0x20, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    iowrite32((WM8731_RIGHT_HP_OUT << 9) | 0x20, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    iowrite32((WM8731_ANALOG_PATH << 9) | 0x12, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    iowrite32((WM8731_DIGITAL_PATH << 9) | 0x00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    /* Set I2S format, 24-bit data */
    iowrite32((WM8731_DIGITAL_IF << 9) | 0x0A, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    iowrite32((WM8731_SAMPLING_CTRL << 9) | 0x00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(2);
    iowrite32((WM8731_ACTIVE_CTRL << 9) | 0x01, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);
    mdelay(5);

    mutex_unlock(&hw_mutex);

    printk(KERN_INFO "WM8731 configured: I2S 24-bit, 48kHz\n");
}

void reset_audio_completely(void)
{
    printk(KERN_INFO "Complete audio reset\n");

    mutex_lock(&hw_mutex);

    iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    mdelay(10);
    iowrite32(0x2, audiobase + AUDIO_CONTROL_REG);
    mdelay(10);
    iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    mdelay(5);

    mutex_unlock(&hw_mutex);
}

void init_audio_ip_via_axi(void)
{
    uint32_t fifospace;
    printk(KERN_INFO "Initializing Audio IP for 48kHz I2S streaming\n");

    reset_audio_completely();

    mutex_lock(&hw_mutex);

    iowrite32(0x1, audiobase + AUDIO_CONTROL_REG);
    mdelay(5);
    fifospace = ioread32(audiobase + AUDIO_FIFOSPACE_REG);

    mutex_unlock(&hw_mutex);

    printk(KERN_INFO "AXI Audio FIFO: 0x%08x\n", fifospace);
    if (fifospace == 0 || fifospace == 0xFFFFFFFF) {
        printk(KERN_WARNING "AXI FIFO not responding, extended reset\n");
        mutex_lock(&hw_mutex);
        iowrite32(0x3, audiobase + AUDIO_CONTROL_REG);
        mdelay(10);
        iowrite32(0x1, audiobase + AUDIO_CONTROL_REG);
        mdelay(5);
        mutex_unlock(&hw_mutex);
    }
    printk(KERN_INFO "AXI Audio IP ready for 48kHz I2S\n");
}

/* WAV parser optimizado */
int parse_wav_header_48khz(wav_file_t* wav)
{
    uint8_t chunk_header[8];
    uint8_t fmt_chunk[16];
    int bytes_read;
    uint32_t chunk_size;
    loff_t current_offset;
    int found_fmt = 0;
    int found_data = 0;
    mm_segment_t oldfs;

    if (!wav->file) {
        printk(KERN_ERR "No file handle for WAV parsing\n");
        return 0;
    }

    printk(KERN_INFO "Parsing WAV file\n");

    /* Read RIFF header */
    current_offset = 0;

    oldfs = get_fs();
    set_fs(KERNEL_DS);
    bytes_read = vfs_read(wav->file, (char __user *)chunk_header, 8, &current_offset);
    set_fs(oldfs);

    if (bytes_read != 8) {
        printk(KERN_ERR "Failed to read RIFF header\n");
        return 0;
    }

    if (memcmp(chunk_header, "RIFF", 4) != 0) {
        printk(KERN_ERR "Invalid RIFF header\n");
        return 0;
    }

    oldfs = get_fs();
    set_fs(KERNEL_DS);
    bytes_read = vfs_read(wav->file, (char __user *)chunk_header, 4, &current_offset);
    set_fs(oldfs);

    if (bytes_read != 4 || memcmp(chunk_header, "WAVE", 4) != 0) {
        printk(KERN_ERR "Invalid WAVE header\n");
        return 0;
    }

    while (current_offset < 200 && (!found_fmt || !found_data)) {
        oldfs = get_fs();
        set_fs(KERNEL_DS);
        bytes_read = vfs_read(wav->file, (char __user *)chunk_header, 8, &current_offset);
        set_fs(oldfs);

        if (bytes_read != 8) {
            printk(KERN_WARNING "End of header at offset %lld\n", current_offset);
            break;
        }

        chunk_size = *(uint32_t*)(chunk_header + 4);

        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            if (chunk_size >= 16) {
                oldfs = get_fs();
                set_fs(KERNEL_DS);
                bytes_read = vfs_read(wav->file, (char __user *)fmt_chunk, 16, &current_offset);
                set_fs(oldfs);

                if (bytes_read == 16) {
                    wav->channels = *(uint16_t*)(fmt_chunk + 2);
                    wav->sample_rate = *(uint32_t*)(fmt_chunk + 4);
                    wav->bits_per_sample = *(uint16_t*)(fmt_chunk + 14);
                    found_fmt = 1;

                    printk(KERN_INFO "WAV Format: %uHz, %uch, %ubit\n",
                           wav->sample_rate, wav->channels, wav->bits_per_sample);
                }
                if (chunk_size > 16) {
                    current_offset += (chunk_size - 16);
                }
            } else {
                current_offset += chunk_size;
            }

        } else if (memcmp(chunk_header, "data", 4) == 0) {
            wav->data_size = chunk_size;
            wav->data_start_offset = current_offset;
            found_data = 1;

            printk(KERN_INFO "Data chunk: %u bytes at offset %u\n",
                   wav->data_size, wav->data_start_offset);
            break;

        } else {
            current_offset += chunk_size;
        }

        if (chunk_size % 2) {
            current_offset++;
        }
    }

    if (!found_fmt || !found_data) {
        printk(KERN_ERR "Missing WAV chunks: fmt=%d data=%d\n", found_fmt, found_data);
        return 0;
    }

    if (wav->channels > 0 && wav->bits_per_sample > 0) {
        uint32_t bytes_per_sample = wav->channels * (wav->bits_per_sample / 8);
        wav->total_samples = wav->data_size / bytes_per_sample;
    } else {
        wav->total_samples = 0;
    }

    wav->samples_played = 0;
    wav->current_pos = wav->data_start_offset;

    printk(KERN_INFO "WAV parsed: %u samples total\n", wav->total_samples);

    return 1;
}

void load_work_handler(struct work_struct *work)
{
    wav_file_t *wav = &songs[next_track_to_load];

    if (wav->file) {
        filp_close(wav->file, NULL);
        wav->file = NULL;
    }

    wav->file = filp_open(song_paths[next_track_to_load], O_RDONLY, 0);
    if (IS_ERR(wav->file)) {
        printk(KERN_ERR "Cannot open %s\n", song_paths[next_track_to_load]);
        wav->file = NULL;
        wav->is_valid = 0;
        return;
    }

    strcpy(wav->filename, song_paths[next_track_to_load]);
    if (!parse_wav_header_48khz(wav)) {
        printk(KERN_ERR "Failed to parse WAV\n");
        filp_close(wav->file, NULL);
        wav->file = NULL;
        wav->is_valid = 0;
        return;
    }

    wav->is_valid = 1;
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    printk(KERN_INFO "Track %d loaded\n", next_track_to_load);
}

void refill_work_handler(struct work_struct *work)
{
    wav_file_t *wav = &songs[current_track];
    int bytes_read;
    mm_segment_t oldfs;
    unsigned long flags;

    if (!wav->is_valid || !wav->file || !buffer_needs_refill)
        return;

    oldfs = get_fs();
    set_fs(KERNEL_DS);
    bytes_read = vfs_read(wav->file, (char __user *)audio_buffer, AUDIO_BUFFER_SIZE, &wav->current_pos);
    set_fs(oldfs);

    spin_lock_irqsave(&buffer_lock, flags);
    if (bytes_read > 0) {
        printk(KERN_INFO "Refilled audio buffer with %d bytes\n", bytes_read);
        buffer_size = bytes_read;
        buffer_pos = 0;
        buffer_needs_refill = 0;
    } else {
        printk(KERN_INFO "End of track reached\n");
        buffer_size = 0;
        buffer_pos = 0;
        buffer_needs_refill = 0;
    }
    spin_unlock_irqrestore(&buffer_lock, flags);
}

void display_time_mmss(void)
{
    uint32_t display_value;
    uint8_t min_tens, min_ones, sec_tens, sec_ones;

    min_tens = (current_time_minutes / 10) % 10;
    min_ones = current_time_minutes % 10;
    sec_tens = (current_time_seconds / 10) % 10;
    sec_ones = current_time_seconds % 10;

    display_value = ((uint32_t)seven_seg_patterns[min_tens] << 21) |
                    ((uint32_t)seven_seg_patterns[min_ones] << 14) |
                    ((uint32_t)seven_seg_patterns[sec_tens] << 7)  |
                    ((uint32_t)seven_seg_patterns[sec_ones]);

    mutex_lock(&hw_mutex);
    iowrite32(display_value, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    mutex_unlock(&hw_mutex);
}

void generate_audio_samples_48khz(void)
{
    uint32_t fifospace;
    int write_space_left, write_space_right, write_space;
    int i;
    wav_file_t *wav;
    unsigned long flags;

    if (current_state != AUDIO_PLAYING) return;

    wav = &songs[current_track];
    if (!wav->is_valid) return;

    /* Verificar si necesitamos recargar el buffer proactivamente */
    spin_lock_irqsave(&buffer_lock, flags);
    if (buffer_pos > buffer_size / 2 && !buffer_needs_refill) {
        buffer_needs_refill = 1;
        queue_work(audio_wq, &refill_work);
    }
    spin_unlock_irqrestore(&buffer_lock, flags);

    mutex_lock(&hw_mutex);
    fifospace = ioread32(audiobase + AUDIO_FIFOSPACE_REG);
    mutex_unlock(&hw_mutex);

    write_space_left = (fifospace >> 24) & 0xFF;
    write_space_right = (fifospace >> 16) & 0xFF;
    write_space = min(write_space_left, write_space_right);

    if (write_space < 4) {
        return;
    }

    spin_lock_irqsave(&buffer_lock, flags);
    
    if (buffer_size == 0 || buffer_pos >= buffer_size) {
        spin_unlock_irqrestore(&buffer_lock, flags);
        return;
    }

    mutex_lock(&hw_mutex);
    for (i = 0; i < write_space && buffer_pos + 3 < buffer_size; i++) {
        static int log_counter = 0;
        if (i > 0 && log_counter++ % 100 == 0) {
            printk(KERN_INFO "Sent %d samples to audio FIFO\n", i);
        }
        int16_t left_16, right_16;
        int32_t left_24, right_24;

        left_16 = *(int16_t*)(&audio_buffer[buffer_pos]);
        right_16 = *(int16_t*)(&audio_buffer[buffer_pos + 2]);
        buffer_pos += 4;
        wav->samples_played++;

        left_24 = ((int32_t)left_16) << 8;
        right_24 = ((int32_t)right_16) << 8;

        iowrite32(left_24, audiobase + AUDIO_LEFTDATA_REG);
        iowrite32(right_24, audiobase + AUDIO_RIGHTDATA_REG);
    }
    mutex_unlock(&hw_mutex);
    if (i > 0) {
        static int log_counter = 0;
        if (log_counter++ % 100 == 0) {
            printk(KERN_INFO "Sent %d samples to audio FIFO\n", i);
        }
    }
    spin_unlock_irqrestore(&buffer_lock, flags);
}

void audio_timer_callback(unsigned long data)
{
    if (current_state == AUDIO_PLAYING) {
        generate_audio_samples_48khz();
        mod_timer(&audio_timer, jiffies + msecs_to_jiffies(1));
    }
}

void display_timer_callback(unsigned long data)
{
    if (current_state == AUDIO_PLAYING) {
        current_time_seconds++;
        if (current_time_seconds >= 60) {
            current_time_seconds = 0;
            current_time_minutes++;
            if (current_time_minutes >= 100) {
                current_time_minutes = 0;
            }
        }
        display_time_mmss();
    }
    mod_timer(&display_timer, jiffies + HZ);
}

int is_button_debounced(int button_num)
{
    struct timespec current_time;
    long long diff_ms;

    getnstimeofday(&current_time);

    diff_ms = (current_time.tv_sec - last_button_time[button_num].tv_sec) * 1000 +
              (current_time.tv_nsec - last_button_time[button_num].tv_nsec) / 1000000;

    if (diff_ms > DEBOUNCE_TIME_MS) {
        last_button_time[button_num] = current_time;
        return 1;
    }
    return 0;
}

void button_work_handler(struct work_struct *work)
{
    uint32_t buttons_to_process = atomic_xchg(&pending_buttons, 0);

    if (!buttons_to_process)
        return;

    printk(KERN_INFO "Button work handler: 0x%x\n", buttons_to_process);

    if ((buttons_to_process & BUTTON_PLAY_PAUSE) && is_button_debounced(0)) {
        if (current_state == AUDIO_PLAYING) {
            audio_pause();
            send_user_command('2');
        } else {
            audio_play();
            send_user_command('1');
        }
    }

    if ((buttons_to_process & BUTTON_NEXT) && is_button_debounced(1)) {
        audio_next_track();
        send_user_command('3');
    }

    if ((buttons_to_process & BUTTON_PREV) && is_button_debounced(2)) {
        audio_prev_track();
        send_user_command('4');
    }
}

static irqreturn_t button_irq_handler(int irq, void *dev_id)
{
    uint32_t edge_capture;

    edge_capture = ioread32(lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);

    if (edge_capture & (BUTTON_PLAY_PAUSE | BUTTON_NEXT | BUTTON_PREV)) {
        atomic_or(edge_capture, &pending_buttons);
        queue_work(audio_wq, &button_work);
    }

    iowrite32(edge_capture, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);

    return IRQ_HANDLED;
}

void audio_play(void)
{
    printk(KERN_INFO "PLAY: Track %d\n", current_track);
    current_state = AUDIO_PLAYING;

    if (songs[current_track].is_valid && buffer_size == 0) {
        buffer_needs_refill = 1;
        queue_work(audio_wq, &refill_work);
        msleep(100);
    }

    init_audio_ip_via_axi();

    mod_timer(&audio_timer, jiffies + msecs_to_jiffies(1));
    mod_timer(&display_timer, jiffies + HZ);
    display_time_mmss();
    printk(KERN_INFO "I2S playback started\n");
}

void audio_pause(void)
{
    printk(KERN_INFO "PAUSE\n");
    current_state = AUDIO_PAUSED;
    reset_audio_completely();
    del_timer(&audio_timer);
    del_timer(&display_timer);
}

void audio_next_track(void)
{
    int was_playing;

    was_playing = (current_state == AUDIO_PLAYING);
    if (was_playing) audio_pause();

    current_track = (current_track + 1) % total_tracks;
    current_time_seconds = 0;
    current_time_minutes = 0;

    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;

    next_track_to_load = current_track;
    queue_work(audio_wq, &load_work);

    printk(KERN_INFO "Next track: %d\n", current_track);
    display_time_mmss();

    if (was_playing) {
        msleep(200);
        audio_play();
    }
}

void audio_prev_track(void)
{
    int was_playing;

    was_playing = (current_state == AUDIO_PLAYING);
    if (was_playing) audio_pause();

    current_track = (current_track - 1 + total_tracks) % total_tracks;
    current_time_seconds = 0;
    current_time_minutes = 0;

    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;

    next_track_to_load = current_track;
    queue_work(audio_wq, &load_work);

    printk(KERN_INFO "Prev track: %d\n", current_track);
    display_time_mmss();

    if (was_playing) {
        msleep(200);
        audio_play();
    }
}

/* Module initialization */
static int __init initialize_48khz_audio_player(void)
{
    int result;
    int i;
    struct timespec now;

    printk(KERN_INFO "=== I2S 24-bit Audio Player ===\n");
    printk(KERN_INFO "48kHz Stereo WAV Support\n");

    getnstimeofday(&now);
    for (i = 0; i < 3; i++)
        last_button_time[i] = now;

    audio_wq = create_singlethread_workqueue("audio_wq");
    if (!audio_wq) {
        printk(KERN_ERR "Failed to create workqueue\n");
        return -ENOMEM;
    }
    INIT_WORK(&load_work, load_work_handler);
    INIT_WORK(&refill_work, refill_work_handler);
    INIT_WORK(&button_work, button_work_handler);


    audio_buffer = kmalloc(AUDIO_BUFFER_SIZE, GFP_KERNEL);
    if (!audio_buffer) {
        printk(KERN_ERR "Failed to allocate audio buffer\n");
        destroy_workqueue(audio_wq);
        return -ENOMEM;
    }

    init_timer(&audio_timer);
    audio_timer.function = audio_timer_callback;
    audio_timer.data = 0;

    init_timer(&display_timer);
    display_timer.function = display_timer_callback;
    display_timer.data = 0;

    lwbridgebase = ioremap_nocache(0xff200000, 0x200000);
    audiobase = ioremap_nocache(0xc0000000, 0x800000);
    if (!lwbridgebase || !audiobase) {
        printk(KERN_ERR "Memory mapping failed\n");
        if (lwbridgebase) iounmap(lwbridgebase);
        if (audiobase) iounmap(audiobase);
        kfree(audio_buffer);
        destroy_workqueue(audio_wq);
        return -ENOMEM;
    }

    printk(KERN_INFO "Memory mapped: LW_AXI=0x%p AXI_AUDIO=0x%p\n",
           lwbridgebase, audiobase);

    iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    init_wm8731_via_lw_axi();

    result = alloc_chrdev_region(&fpga_cmd_dev, 0, 1, "fpga_cmd");
    if (result < 0) {
        printk(KERN_ERR "alloc_chrdev_region failed\n");
        goto err_unmap;
    }
    cdev_init(&fpga_cmd_cdev, &fpga_cmd_fops);
    if (cdev_add(&fpga_cmd_cdev, fpga_cmd_dev, 1)) {
        printk(KERN_ERR "cdev_add failed\n");
        unregister_chrdev_region(fpga_cmd_dev, 1);
        goto err_unmap;
    }
    fpga_cmd_class = class_create(THIS_MODULE, "fpga_cmd_class");
    if (IS_ERR(fpga_cmd_class)) {
        printk(KERN_ERR "class_create failed\n");
        cdev_del(&fpga_cmd_cdev);
        unregister_chrdev_region(fpga_cmd_dev, 1);
        goto err_unmap;
    }
    fpga_cmd_device = device_create(fpga_cmd_class, NULL, fpga_cmd_dev, NULL, "fpga_cmd");
    if (IS_ERR(fpga_cmd_device)) {
        printk(KERN_ERR "device_create failed\n");
        class_destroy(fpga_cmd_class);
        cdev_del(&fpga_cmd_cdev);
        unregister_chrdev_region(fpga_cmd_dev, 1);
        goto err_unmap;
    }

    printk(KERN_INFO "fpga_cmd device created: major=%d minor=%d\n", MAJOR(fpga_cmd_dev), MINOR(fpga_cmd_dev));

    current_time_seconds = 0;
    current_time_minutes = 0;
    display_time_mmss();

    next_track_to_load = 0;
    queue_work(audio_wq, &load_work);
    msleep(200);

    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
    result = request_irq(72 + 4, (irq_handler_t)button_irq_handler,
                        IRQF_SHARED, "audio_48khz_irq", (void *)(button_irq_handler));
    if (result) {
        printk(KERN_ERR "IRQ request failed: %d\n", result);
        device_destroy(fpga_cmd_class, fpga_cmd_dev);
        class_destroy(fpga_cmd_class);
        cdev_del(&fpga_cmd_cdev);
        unregister_chrdev_region(fpga_cmd_dev, 1);
        goto err_unmap;
    }

    printk(KERN_INFO "=== 48kHz Audio Player Ready (OLD KERNEL) ===\n");
    printk(KERN_INFO "Button 0: Play/Pause\n");
    printk(KERN_INFO "Button 1: Next Track\n");
    printk(KERN_INFO "Button 2: Previous Track\n");
    return 0;

err_unmap:
    if (lwbridgebase) iounmap(lwbridgebase);
    if (audiobase) iounmap(audiobase);
    kfree(audio_buffer);
    destroy_workqueue(audio_wq);
    return -ENOMEM;
}

static void __exit cleanup_48khz_audio_player(void)
{
    int i;

    printk(KERN_INFO "=== 48kHz cleanup ===\n");

    current_state = AUDIO_STOPPED;

    del_timer_sync(&audio_timer);
    del_timer_sync(&display_timer);

    if (audio_wq) {
        flush_workqueue(audio_wq);
        destroy_workqueue(audio_wq);
    }

    if (audio_buffer)
        kfree(audio_buffer);

    for (i = 0; i < total_tracks; i++) {
        if (songs[i].file) {
            filp_close(songs[i].file, NULL);
            songs[i].file = NULL;
        }
    }

    if (lwbridgebase) {
        iowrite32(0x0, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
        iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    }

    free_irq(72 + 4, (void*)button_irq_handler);

    if (fpga_cmd_device && fpga_cmd_class)
        device_destroy(fpga_cmd_class, fpga_cmd_dev);

    if (fpga_cmd_class)
        class_destroy(fpga_cmd_class);

    cdev_del(&fpga_cmd_cdev);
    unregister_chrdev_region(fpga_cmd_dev, 1);

    if (lwbridgebase) iounmap(lwbridgebase);
    if (audiobase) iounmap(audiobase);

    printk(KERN_INFO "=== I2S 24-bit Audio Player cleanup ===\n");
}

module_init(initialize_48khz_audio_player);
module_exit(cleanup_48khz_audio_player);