/*----------------------------------------------*/
/* TJpgDec System Configurations R0.03          */
/*----------------------------------------------*/
/* Snapix-tuned config for ESP32-C3 RV32IMC.     */

#define JD_SZBUF        4096
/* Stream input buffer size.  Sized to one SDFat block so each refill
 * acquires SharedBusLock once and pulls a full FAT cluster — same
 * reasoning as the picojpeg buffer bump (cuts ~8× lock acquires on a
 * typical 150 KB JPEG).  Costs 4 KB heap per active decode. */

#define JD_FORMAT       2
/* Output pixel format.
 *   0: RGB888 (24-bit/pix) — wasted bandwidth, we only need luminance
 *   1: RGB565 (16-bit/pix) — same problem
 *   2: Grayscale (8-bit/pix) — exactly what the e-ink renderer wants;
 *      saves 3× memory bandwidth and skips the YCbCr→RGB color
 *      conversion stage entirely.
 */

#define JD_USE_SCALE    1
/* Enable jd_decomp's scale parameter (0=1/1, 1=1/2, 2=1/4, 3=1/8).
 * Mandatory for our progressive preview path. */

#define JD_TBLCLIP      1
/* Lookup-table saturation arithmetic — ~1 KB ROM for ~5 % decode speed.
 * Cheap trade on a 6.5 MB partition. */

#define JD_FASTDECODE   2
/* 0: 8/16-bit MCU optimisation, ~3.1 KB workspace.
 * 1: 32-bit barrel shifter, ~3.5 KB workspace — fine for ESP32-C3.
 * 2: + Huffman LUT, ~9.6 KB workspace.  ~2× decode speedup over level 1
 *    on baseline JPEGs with rich Huffman tables.  ESP32-C3 has 320 KB
 *    SRAM; 10 KB workspace + 4 KB input buffer is well within budget.
 */

// Do not change this, it is the minimum size in bytes of the workspace needed by the decoder
#if JD_FASTDECODE == 0
 #define TJPGD_WORKSPACE_SIZE 3100
#elif JD_FASTDECODE == 1
 #define TJPGD_WORKSPACE_SIZE 3500
#elif JD_FASTDECODE == 2
 #define TJPGD_WORKSPACE_SIZE (3500 + 6144)
#endif
