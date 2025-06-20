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

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Audio Player Team");
MODULE_DESCRIPTION("Hybrid Dual AXI Audio Player - Dynamic WAV Parser");

// Virtual address bases
void *lwbridgebase;  // Para controles (botones, display, WM8731)
void *audiobase;     // Para avalon_audio_slave (AXI Master)

// Audio player state
typedef enum {
    AUDIO_PLAYING,
    AUDIO_PAUSED,
    AUDIO_STOPPED
} audio_state_t;

static audio_state_t current_state = AUDIO_STOPPED;
static int current_track = 0;
static const int total_tracks = 3;

// WAV file structure
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
    "/media/sd/songs/song1.wav",
    "/media/sd/songs/song2.wav", 
    "/media/sd/songs/song3.wav"
};

// Time tracking
static int current_time_seconds = 0;
static int current_time_minutes = 0;
static struct timer_list audio_timer;
static struct timer_list display_timer;

// Button definitions
#define BUTTON_PLAY_PAUSE   0x1
#define BUTTON_NEXT         0x2
#define BUTTON_PREV         0x4

// LW AXI Master offsets (controles)
#define BUTTONS_BASE_OFFSET     0x8800
#define BUTTONS_DATA_OFFSET     0x0
#define BUTTONS_INTERRUPT_MASK  0x8
#define BUTTONS_EDGE_CAPTURE    0xC
#define SEVEN_SEGMENTS_BASE_OFFSET 0x8810
#define AUDIO_CONFIG_BASE_OFFSET    0x8850

// AXI Master offsets (solo audio data)
#define AUDIO_FIFOSPACE_REG     0x4
#define AUDIO_LEFTDATA_REG      0x8
#define AUDIO_RIGHTDATA_REG     0xC
#define AUDIO_CONTROL_REG       0x0

// Audio buffer
#define AUDIO_BUFFER_SIZE 8192
static uint8_t audio_buffer[AUDIO_BUFFER_SIZE];
static int buffer_pos = 0;
static int buffer_size = 0;
static int buffer_needs_refill = 1;

// Sample rate matching
static int sample_skip_counter = 0;

// Workqueue for file operations
static struct workqueue_struct *audio_wq;
static struct work_struct load_work;
static struct work_struct refill_work;
static int next_track_to_load = 0;

// Debouncing
static struct timespec last_button_time[3];
#define DEBOUNCE_TIME_MS 200

// 7-segment patterns
static const uint8_t seven_seg_patterns[10] = {
    0x40, 0x79, 0x24, 0x30, 0x19,
    0x12, 0x02, 0x78, 0x00, 0x10
};

// Function declarations
void audio_play(void);
void audio_pause(void);
void audio_next_track(void);
void audio_prev_track(void);
void display_time_mmss(void);
void generate_audio_samples_hybrid(void);
void audio_timer_callback(unsigned long data);
void display_timer_callback(unsigned long data);
int is_button_debounced(int button_num);
void load_work_handler(struct work_struct *work);
void refill_work_handler(struct work_struct *work);
int parse_wav_header_dynamic(wav_file_t* wav);
void init_wm8731_via_lw_axi(void);
void init_audio_ip_via_axi(void);
void reset_audio_completely(void);

// WM8731 initialization via LW AXI Master
void init_wm8731_via_lw_axi(void)
{
    printk(KERN_INFO "Initializing WM8731 via LW AXI Master\n");
    
    iowrite32(0x1E00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);  // Reset
    iowrite32(0x0C00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);  // Power up
    iowrite32(0x047F, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);  // Left volume
    iowrite32(0x067F, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);  // Right volume
    iowrite32(0x0812, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);  // Analog path
    iowrite32(0x0A00, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);  // Digital path
    iowrite32(0x0E42, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);  // I2S format
    iowrite32(0x1000, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);  // 48kHz
    iowrite32(0x1201, lwbridgebase + AUDIO_CONFIG_BASE_OFFSET);  // Active
    
    printk(KERN_INFO "WM8731 initialized\n");
}

// Complete audio reset
void reset_audio_completely(void)
{
    printk(KERN_INFO "Complete audio reset\n");
    
    // Reset audio IP via AXI Master
    iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    mdelay(10);
    iowrite32(0x2, audiobase + AUDIO_CONTROL_REG);  // Full reset
    mdelay(10);
    iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    mdelay(10);
}

// Audio IP initialization via AXI Master
void init_audio_ip_via_axi(void)
{
    uint32_t fifospace;
    
    printk(KERN_INFO "Initializing Audio IP via AXI Master\n");
    
    // Complete reset first
    reset_audio_completely();
    
    // Enable
    iowrite32(0x1, audiobase + AUDIO_CONTROL_REG);
    mdelay(5);
    
    fifospace = ioread32(audiobase + AUDIO_FIFOSPACE_REG);
    printk(KERN_INFO "AXI Audio FIFO: 0x%08x\n", fifospace);
    
    if (fifospace == 0 || fifospace == 0xFFFFFFFF) {
        printk(KERN_WARNING "AXI FIFO not responding, trying extended reset\n");
        iowrite32(0x3, audiobase + AUDIO_CONTROL_REG);
        mdelay(10);
        iowrite32(0x1, audiobase + AUDIO_CONTROL_REG);
        mdelay(5);
        fifospace = ioread32(audiobase + AUDIO_FIFOSPACE_REG);
        printk(KERN_INFO "AXI FIFO after extended reset: 0x%08x\n", fifospace);
    }
    
    printk(KERN_INFO "AXI Audio IP ready\n");
}

// DYNAMIC WAV PARSER - No fixed offsets
int parse_wav_header_dynamic(wav_file_t* wav)
{
    uint8_t chunk_header[8];
    uint8_t fmt_chunk[16];
    mm_segment_t oldfs;
    int bytes_read;
    uint32_t chunk_size;
    loff_t current_offset;
    int found_fmt = 0;
    int found_data = 0;
    
    if (!wav->file) return 0;
    
    oldfs = get_fs();
    set_fs(KERNEL_DS);
    
    // Read RIFF header
    current_offset = 0;
    bytes_read = vfs_read(wav->file, chunk_header, 8, &current_offset);
    if (bytes_read != 8) {
        printk(KERN_ERR "Failed to read RIFF header\n");
        set_fs(oldfs);
        return 0;
    }
    
    // Verify RIFF
    if (memcmp(chunk_header, "RIFF", 4) != 0) {
        printk(KERN_ERR "No RIFF header found\n");
        set_fs(oldfs);
        return 0;
    }
    
    // Read WAVE format
    bytes_read = vfs_read(wav->file, chunk_header, 4, &current_offset);
    if (bytes_read != 4 || memcmp(chunk_header, "WAVE", 4) != 0) {
        printk(KERN_ERR "No WAVE header found\n");
        set_fs(oldfs);
        return 0;
    }
    
    printk(KERN_INFO "Dynamic WAV parser: Searching for chunks...\n");
    
    // Search for fmt and data chunks
    while (current_offset < 1000 && (!found_fmt || !found_data)) {
        // Read chunk header (4 bytes ID + 4 bytes size)
        bytes_read = vfs_read(wav->file, chunk_header, 8, &current_offset);
        if (bytes_read != 8) {
            printk(KERN_WARNING "End of header reached at offset %lld\n", current_offset);
            break;
        }
        
        chunk_size = *(uint32_t*)(chunk_header + 4);
        
        printk(KERN_INFO "Found chunk: '%.4s' size=%u at offset=%lld\n", 
               chunk_header, chunk_size, current_offset - 8);
        
        if (memcmp(chunk_header, "fmt ", 4) == 0) {
            // Found format chunk
            if (chunk_size >= 16) {
                bytes_read = vfs_read(wav->file, fmt_chunk, 16, &current_offset);
                if (bytes_read == 16) {
                    wav->channels = *(uint16_t*)(fmt_chunk + 2);
                    wav->sample_rate = *(uint32_t*)(fmt_chunk + 4);
                    wav->bits_per_sample = *(uint16_t*)(fmt_chunk + 14);
                    found_fmt = 1;
                    
                    printk(KERN_INFO "FMT parsed: %uHz, %uch, %ubit\n", 
                           wav->sample_rate, wav->channels, wav->bits_per_sample);
                }
                // Skip any remaining fmt data
                if (chunk_size > 16) {
                    current_offset += (chunk_size - 16);
                }
            } else {
                current_offset += chunk_size;
            }
        } else if (memcmp(chunk_header, "data", 4) == 0) {
            // Found data chunk
            wav->data_size = chunk_size;
            wav->data_start_offset = current_offset;
            found_data = 1;
            
            printk(KERN_INFO "DATA found: size=%u bytes at offset=%u\n", 
                   wav->data_size, wav->data_start_offset);
            
            break;  // No need to continue
        } else {
            // Skip unknown chunk
            printk(KERN_INFO "Skipping chunk '%.4s' (%u bytes)\n", chunk_header, chunk_size);
            current_offset += chunk_size;
        }
        
        // Align to even boundary (WAV standard)
        if (chunk_size % 2) {
            current_offset++;
        }
    }
    
    set_fs(oldfs);
    
    if (!found_fmt || !found_data) {
        printk(KERN_ERR "Missing chunks: fmt=%d, data=%d\n", found_fmt, found_data);
        return 0;
    }
    
    // Calculate total samples
    if (wav->channels > 0 && wav->bits_per_sample > 0) {
        uint32_t bytes_per_sample = wav->channels * (wav->bits_per_sample / 8);
        wav->total_samples = wav->data_size / bytes_per_sample;
    } else {
        wav->total_samples = 0;
    }
    
    wav->samples_played = 0;
    wav->current_pos = wav->data_start_offset;
    
    printk(KERN_INFO "Dynamic parse complete: %uHz, %uch, %ubit\n", 
           wav->sample_rate, wav->channels, wav->bits_per_sample);
    printk(KERN_INFO "Data: %u KB, Samples: %u, Start offset: %u\n",
           wav->data_size / 1024, wav->total_samples, wav->data_start_offset);
    
    return 1;
}

// Workqueue handlers
void load_work_handler(struct work_struct *work)
{
    wav_file_t *wav;
    
    if (next_track_to_load < 0 || next_track_to_load >= total_tracks) 
        return;
    
    wav = &songs[next_track_to_load];
    
    // Close previous file
    if (wav->file) {
        filp_close(wav->file, NULL);
        wav->file = NULL;
    }
    
    // Open new file
    wav->file = filp_open(song_paths[next_track_to_load], O_RDONLY, 0);
    if (IS_ERR(wav->file)) {
        printk(KERN_ERR "Cannot open %s (error %ld)\n", 
               song_paths[next_track_to_load], PTR_ERR(wav->file));
        wav->file = NULL;
        wav->is_valid = 0;
        return;
    }
    
    strcpy(wav->filename, song_paths[next_track_to_load]);
    
    // Use dynamic parser
    if (!parse_wav_header_dynamic(wav)) {
        filp_close(wav->file, NULL);
        wav->file = NULL;
        wav->is_valid = 0;
        return;
    }
    
    wav->is_valid = 1;
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    
    printk(KERN_INFO "Successfully loaded: %s\n", wav->filename);
}

void refill_work_handler(struct work_struct *work)
{
    wav_file_t *wav = &songs[current_track];
    mm_segment_t oldfs;
    int bytes_read;
    
    if (!wav->is_valid || !wav->file || !buffer_needs_refill) 
        return;
    
    oldfs = get_fs();
    set_fs(KERNEL_DS);
    
    bytes_read = vfs_read(wav->file, audio_buffer, AUDIO_BUFFER_SIZE, &wav->current_pos);
    
    set_fs(oldfs);
    
    if (bytes_read > 0) {
        buffer_size = bytes_read;
        buffer_pos = 0;
        buffer_needs_refill = 0;
        
        static int refill_log_counter = 0;
        if (++refill_log_counter % 10 == 0) {
            printk(KERN_INFO "Buffer refilled: %d bytes (every 10th logged)\n", bytes_read);
        }
    } else {
        printk(KERN_INFO "End of file reached\n");
        buffer_size = 0;
        buffer_pos = 0;
    }
}

// Display time via LW AXI Master
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
                   ((uint32_t)seven_seg_patterns[sec_tens] << 7) |
                   ((uint32_t)seven_seg_patterns[sec_ones]);
    
    iowrite32(display_value, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
}

// HYBRID audio generation with sample rate conversion
void generate_audio_samples_hybrid(void)
{
    uint32_t fifospace;
    int write_space_left, write_space_right;
    int i;
    wav_file_t *wav;
    
    if (current_state != AUDIO_PLAYING) return;
    
    wav = &songs[current_track];
    if (!wav->is_valid) return;
    
    // Read FIFO space via AXI Master
    fifospace = ioread32(audiobase + AUDIO_FIFOSPACE_REG);
    write_space_left = (fifospace >> 24) & 0xFF;
    write_space_right = (fifospace >> 16) & 0xFF;
    
    if (write_space_left < 5 || write_space_right < 5) return;
    
    // Check if we need to refill buffer
    if (buffer_pos >= buffer_size && !buffer_needs_refill) {
        buffer_needs_refill = 1;
        queue_work(audio_wq, &refill_work);
        return;
    }
    
    // Process samples with sample rate conversion
    for (i = 0; i < 10 && write_space_left > 0 && write_space_right > 0; i++) {
        uint32_t sample_32 = 0;
        int16_t left_16, right_16;
        
        // Sample rate conversion: 8000Hz -> 48000Hz (6x upsampling)
        sample_skip_counter++;
        if (wav->sample_rate == 8000 && sample_skip_counter < 6) {
            // Repeat same sample 6 times for 8000->48000 conversion
            continue;
        }
        sample_skip_counter = 0;
        
        // Check if we have data in buffer
        if (buffer_pos + 1 < buffer_size) {
            if (wav->bits_per_sample == 16) {
                // 16-bit samples
                left_16 = *(int16_t*)(&audio_buffer[buffer_pos]);
                if (wav->channels == 2) {
                    right_16 = *(int16_t*)(&audio_buffer[buffer_pos + 2]);
                    buffer_pos += 4;
                } else {
                    right_16 = left_16;  // Mono to stereo
                    buffer_pos += 2;
                }
            } else {
                // 8-bit samples (convert to 16-bit)
                left_16 = ((int16_t)audio_buffer[buffer_pos] - 128) * 256;
                if (wav->channels == 2) {
                    right_16 = ((int16_t)audio_buffer[buffer_pos + 1] - 128) * 256;
                    buffer_pos += 2;
                } else {
                    right_16 = left_16;
                    buffer_pos += 1;
                }
            }
            wav->samples_played++;
        } else {
            // Use silence if no data available
            left_16 = 0;
            right_16 = 0;
        }
        
        // Reduce volume to prevent distortion
        left_16 = left_16 / 4;
        right_16 = right_16 / 4;
        
        // Pack into 32-bit sample
        sample_32 = ((uint32_t)(left_16 & 0xFFFF) << 16) | (right_16 & 0xFFFF);
        
        // Write to audio hardware via AXI Master
        iowrite32(sample_32, audiobase + AUDIO_LEFTDATA_REG);
        iowrite32(sample_32, audiobase + AUDIO_RIGHTDATA_REG);
        
        write_space_left--;
        write_space_right--;
    }
}

// Timer callbacks
void audio_timer_callback(unsigned long data)
{
    if (current_state == AUDIO_PLAYING) {
        generate_audio_samples_hybrid();
        mod_timer(&audio_timer, jiffies + msecs_to_jiffies(10));
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

// Debouncing
int is_button_debounced(int button_num)
{
    struct timespec current_time;
    long diff_ms;
    
    getnstimeofday(&current_time);
    
    diff_ms = (current_time.tv_sec - last_button_time[button_num].tv_sec) * 1000 +
              (current_time.tv_nsec - last_button_time[button_num].tv_nsec) / 1000000;
    
    if (diff_ms > DEBOUNCE_TIME_MS) {
        last_button_time[button_num] = current_time;
        return 1;
    }
    return 0;
}

// IRQ Handler - via LW AXI Master
irq_handler_t button_irq_handler(int irq, void *dev_id, struct pt_regs *regs)
{
    uint32_t button_value;
    uint32_t edge_capture;
    
    edge_capture = ioread32(lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    button_value = ioread32(lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_DATA_OFFSET);
    
    printk(KERN_INFO "Button IRQ: edge=0x%x, data=0x%x\n", edge_capture, button_value);
    
    if ((edge_capture & BUTTON_PLAY_PAUSE) && is_button_debounced(0)) {
        if (current_state == AUDIO_PLAYING) {
            audio_pause();
        } else {
            audio_play();
        }
    }
    
    if ((edge_capture & BUTTON_NEXT) && is_button_debounced(1)) {
        audio_next_track();
    }
    
    if ((edge_capture & BUTTON_PREV) && is_button_debounced(2)) {
        audio_prev_track();
    }
    
    iowrite32(edge_capture, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    return (irq_handler_t) IRQ_HANDLED;
}

// Audio control functions
void audio_play(void)
{
    printk(KERN_INFO "HYBRID PLAY: Track %d\n", current_track);
    
    current_state = AUDIO_PLAYING;
    sample_skip_counter = 0;
    
    // Ensure we have initial data
    if (songs[current_track].is_valid && buffer_size == 0) {
        buffer_needs_refill = 1;
        queue_work(audio_wq, &refill_work);
        mdelay(100);
    }
    
    // Initialize audio hardware via AXI Master
    init_audio_ip_via_axi();
    
    // Start timers
    mod_timer(&audio_timer, jiffies + msecs_to_jiffies(10));
    mod_timer(&display_timer, jiffies + HZ);
    
    display_time_mmss();
    
    printk(KERN_INFO "Hybrid playback started with dynamic parser\n");
}

void audio_pause(void)
{
    printk(KERN_INFO "HYBRID PAUSE\n");
    current_state = AUDIO_PAUSED;
    
    // Complete reset to prevent accumulation
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
    
    // Reset buffer
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    
    next_track_to_load = current_track;
    queue_work(audio_wq, &load_work);
    
    printk(KERN_INFO "Next track: %d\n", current_track);
    display_time_mmss();
    
    if (was_playing) {
        mdelay(300);
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
    
    // Reset buffer
    buffer_pos = 0;
    buffer_size = 0;
    buffer_needs_refill = 1;
    
    next_track_to_load = current_track;
    queue_work(audio_wq, &load_work);
    
    printk(KERN_INFO "Prev track: %d\n", current_track);
    display_time_mmss();
    
    if (was_playing) {
        mdelay(300);
        audio_play();
    }
}

// Module initialization
static int __init initialize_hybrid_audio_player(void)
{
    int result;
    int i;
    struct timespec current_time;
    
    printk(KERN_INFO "=== Hybrid Dual AXI Audio Player - Dynamic Parser ===\n");
    printk(KERN_INFO "LW AXI: Buttons, Display, WM8731 Config\n");
    printk(KERN_INFO "AXI Master: Audio Data Stream with Sample Rate Conversion\n");
    
    // Initialize debounce
    getnstimeofday(&current_time);
    for (i = 0; i < 3; i++) {
        last_button_time[i] = current_time;
    }
    
    // Create workqueue
    audio_wq = create_singlethread_workqueue("hybrid_audio_wq");
    if (!audio_wq) {
        printk(KERN_ERR "Failed to create workqueue\n");
        return -ENOMEM;
    }
    
    INIT_WORK(&load_work, load_work_handler);
    INIT_WORK(&refill_work, refill_work_handler);
    
    // Initialize timers
    init_timer(&audio_timer);
    audio_timer.function = audio_timer_callback;
    
    init_timer(&display_timer);
    display_timer.function = display_timer_callback;
    
    // Map memory regions
    lwbridgebase = ioremap_nocache(0xff200000, 0x200000);
    audiobase = ioremap_nocache(0xc0000000, 0x500000);
    
    if (!lwbridgebase || !audiobase) {
        printk(KERN_ERR "Memory mapping failed\n");
        if (lwbridgebase) iounmap(lwbridgebase);
        if (audiobase) iounmap(audiobase);
        if (audio_wq) destroy_workqueue(audio_wq);
        return -ENOMEM;
    }
    
    printk(KERN_INFO "Memory mapped: LW_AXI=0x%p, AXI_AUDIO=0x%p (5MB)\n", 
           lwbridgebase, audiobase);
    
    // Initialize hardware
    iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    
    // Initialize WM8731 via LW AXI Master
    init_wm8731_via_lw_axi();
    
    current_time_seconds = 0;
    current_time_minutes = 0;
    display_time_mmss();
    
    // Load first song with dynamic parser
    next_track_to_load = 0;
    queue_work(audio_wq, &load_work);
    mdelay(200);
    
    // Setup interrupts via LW AXI Master
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_EDGE_CAPTURE);
    iowrite32(0x7, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
    
    result = request_irq(72 + 4, (irq_handler_t)button_irq_handler, 
                        IRQF_SHARED, "hybrid_audio_irq", 
                        (void *)(button_irq_handler));
    
    if (result) {
        printk(KERN_ERR "IRQ request failed: %d\n", result);
        if (audio_wq) {
            flush_workqueue(audio_wq);
            destroy_workqueue(audio_wq);
        }
        iounmap(lwbridgebase);
        iounmap(audiobase);
        return result;
    }
    
    printk(KERN_INFO "=== Dynamic Parser Audio Player Ready ===\n");
    printk(KERN_INFO "Press Button 0: Play/Pause\n");
    printk(KERN_INFO "Press Button 1: Next Track\n");
    printk(KERN_INFO "Press Button 2: Previous Track\n");
    
    return 0;
}

// Module cleanup
static void __exit cleanup_hybrid_audio_player(void)
{
    int i;
    
    printk(KERN_INFO "=== Hybrid cleanup ===\n");
    
    current_state = AUDIO_STOPPED;
    del_timer_sync(&audio_timer);
    del_timer_sync(&display_timer);
    
    if (audiobase) {
        iowrite32(0x0, audiobase + AUDIO_CONTROL_REG);
    }
    
    if (audio_wq) {
        flush_workqueue(audio_wq);
        destroy_workqueue(audio_wq);
    }
    
    for (i = 0; i < total_tracks; i++) {
        if (songs[i].file) {
            filp_close(songs[i].file, NULL);
        }
    }
    
    if (lwbridgebase) {
        iowrite32(0x0, lwbridgebase + BUTTONS_BASE_OFFSET + BUTTONS_INTERRUPT_MASK);
        iowrite32(0x0, lwbridgebase + SEVEN_SEGMENTS_BASE_OFFSET);
    }
    
    free_irq(72 + 4, (void*)button_irq_handler);
    
    if (lwbridgebase) iounmap(lwbridgebase);
    if (audiobase) iounmap(audiobase);
    
    printk(KERN_INFO "=== Hybrid cleanup complete ===\n");
}

module_init(initialize_hybrid_audio_player);
module_exit(cleanup_hybrid_audio_player);