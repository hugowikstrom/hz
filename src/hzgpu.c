// SPDX-License-Identifier: GPL-2.0-only
/* hzgpu — nvpack's GPU mode: Vulkan compute on the integrated graphics.
 *
 * Why it exists: the question was whether a GPU mode makes packing faster.
 * That question cannot be answered with an opinion: nvpack does four kinds
 * of work, and only two of them have a GPU shape.  This program runs both
 * of those on the GPU and on the CPU with the same semantics, compares the
 * answers bit for bit and measures the time, so the answer is a measurement
 * instead of an assumption.
 *
 *   hzgpu device           which device was picked and what it can do
 *   hzgpu band [MiB]       bandwidth ceiling: GPU reads vs CPU reads
 *   hzgpu hash             blake2b-128 per file (paths on stdin), bit-exact
 *   hzgpu matcher FILE     LZ77 longest-match: GPU vs CPU, bit for bit
 *   hzgpu selftest         known test vectors + GPU==CPU
 *
 * Flags: --cpu (CPU path only), --quiet, --jobs N (CPU threads),
 *        --window N, --chain N (the matcher's window and chain limit).
 * The original Swedish command and flag names are kept as aliases because
 * existing scripts call them.
 *
 * Bit-exactness: the hash must be exactly the function the python side uses
 * (hashlib.blake2b, digest_size=16), or dedup points at the wrong objects
 * and unpack's object-hash check fails.  That is why blake2b exists in C
 * here too, and why `selftest` runs GPU against CPU against the official
 * test vectors.
 *
 * The matcher is nvpack's own — it need not resemble zlib's, because no
 * unpacker outside nvpack will ever read it.  It searches buckets (one per
 * 3-byte hash) built on the CPU in ascending order, and picks the longest
 * match, nearest on a tie.  GPU and CPU walk the same candidates in the
 * same order, so the outputs must be identical — which is exactly what
 * `matcher` verifies before it reports any timing.
 */
#define _GNU_SOURCE
#include <vulkan/vulkan.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <limits.h>

/* ----------------------------------------------------------------------- */
/* Small helpers                                                           */
/* ----------------------------------------------------------------------- */

static int quiet = 0;

static void die(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("hzgpu: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(2);
}

static void say(const char *fmt, ...) {
    if (quiet) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap); va_end(ap); fflush(stdout);
}

static double now(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

#define VK(expr) do { VkResult _r = (expr); \
    if (_r != VK_SUCCESS) die("%s line %d: vulkan error %d", __func__, __LINE__, (int)_r); } while (0)

static void *zalloc(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) die("out of memory (%zu bytes)", n);
    return p;
}

/* ----------------------------------------------------------------------- */
/* blake2b-128 on the CPU — the reference the GPU must hit                 */
/* ----------------------------------------------------------------------- */

static const uint64_t B2B_IV[8] = {
    0x6a09e667f3bcc908ULL, 0xbb67ae8584caa73bULL, 0x3c6ef372fe94f82bULL,
    0xa54ff53a5f1d36f1ULL, 0x510e527fade682d1ULL, 0x9b05688c2b3e6c1fULL,
    0x1f83d9abfb41bd6bULL, 0x5be0cd19137e2179ULL };

static const uint8_t B2B_SIGMA[12][16] = {
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15},
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3},
    {11, 8,12, 0, 5, 2,15,13,10,14, 3, 6, 7, 1, 9, 4},
    { 7, 9, 3, 1,13,12,11,14, 2, 6, 5,10, 4, 0,15, 8},
    { 9, 0, 5, 7, 2, 4,10,15,14, 1,11,12, 6, 8, 3,13},
    { 2,12, 6,10, 0,11, 8, 3, 4,13, 7, 5,15,14, 1, 9},
    {12, 5, 1,15,14,13, 4,10, 0, 7, 6, 3, 9, 2, 8,11},
    {13,11, 7,14,12, 1, 3, 9, 5, 0,15, 4, 8, 6, 2,10},
    { 6,15,14, 9,11, 3, 0, 8,12, 2,13, 7, 1, 4,10, 5},
    {10, 2, 8, 4, 7, 6, 1, 5,15,11, 9,14, 3,12,13, 0},
    { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9,10,11,12,13,14,15},
    {14,10, 4, 8, 9,15,13, 6, 1,12, 0, 2,11, 7, 5, 3} };

static inline uint64_t rotr64(uint64_t x, int n) { return (x >> n) | (x << (64 - n)); }

static void b2b_compress(uint64_t h[8], const uint8_t blk[128], uint64_t t, int last) {
    uint64_t v[16], m[16];
    for (int i = 0; i < 16; i++) {
        uint64_t w = 0;
        for (int j = 7; j >= 0; j--) w = (w << 8) | blk[i * 8 + j];
        m[i] = w;
    }
    for (int i = 0; i < 8; i++) v[i] = h[i];
    for (int i = 0; i < 8; i++) v[8 + i] = B2B_IV[i];
    v[12] ^= t;
    if (last) v[14] = ~v[14];
#define G(a,b,c,d,x,y) do { \
    v[a] = v[a] + v[b] + (x); v[d] = rotr64(v[d] ^ v[a], 32); \
    v[c] = v[c] + v[d];       v[b] = rotr64(v[b] ^ v[c], 24); \
    v[a] = v[a] + v[b] + (y); v[d] = rotr64(v[d] ^ v[a], 16); \
    v[c] = v[c] + v[d];       v[b] = rotr64(v[b] ^ v[c], 63); } while (0)
    for (int r = 0; r < 12; r++) {
        const uint8_t *s = B2B_SIGMA[r];
        G(0, 4,  8, 12, m[s[0]],  m[s[1]]);
        G(1, 5,  9, 13, m[s[2]],  m[s[3]]);
        G(2, 6, 10, 14, m[s[4]],  m[s[5]]);
        G(3, 7, 11, 15, m[s[6]],  m[s[7]]);
        G(0, 5, 10, 15, m[s[8]],  m[s[9]]);
        G(1, 6, 11, 12, m[s[10]], m[s[11]]);
        G(2, 7,  8, 13, m[s[12]], m[s[13]]);
        G(3, 4,  9, 14, m[s[14]], m[s[15]]);
    }
#undef G
    for (int i = 0; i < 8; i++) h[i] ^= v[i] ^ v[8 + i];
}

/* blake2b, digest 16 bytes, no key — same as hashlib.blake2b(digest_size=16) */
static void blake2b128(const uint8_t *data, size_t n, uint8_t out[16]) {
    uint64_t h[8];
    for (int i = 0; i < 8; i++) h[i] = B2B_IV[i];
    h[0] ^= 0x01010000ULL ^ 16ULL;          /* fanout=1, depth=1, keylen=0, outlen=16 */
    size_t i = 0;
    uint64_t t = 0;
    while (n - i > 128) {                    /* all full blocks except the last */
        t += 128;
        b2b_compress(h, data + i, t, 0);
        i += 128;
    }
    uint8_t last[128];
    memset(last, 0, 128);
    size_t left = n - i;
    memcpy(last, data + i, left);
    t += left;
    b2b_compress(h, last, t, 1);
    for (int k = 0; k < 16; k++) out[k] = (uint8_t)(h[k / 8] >> (8 * (k % 8)));
}

/* ----------------------------------------------------------------------- */
/* Vulkan plumbing                                                         */
/* ----------------------------------------------------------------------- */

typedef struct {
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    VkQueue queue;
    uint32_t queue_family;
    VkCommandPool pool;
    VkPhysicalDeviceProperties props;
    VkPhysicalDeviceMemoryProperties mem;
    uint32_t subgroup;
    char spvdir[PATH_MAX];
} Gpu;

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *p;                 /* mapped the whole time — on an APU it is the same RAM */
    VkDeviceSize size;
    int device_local;        /* landed in DEVICE_LOCAL as well */
} Buf;

typedef struct {
    VkDescriptorSetLayout set_layout;
    VkPipelineLayout layout;
    VkPipeline pipe;
    VkDescriptorPool dpool;
    VkDescriptorSet dset;
    uint32_t nbindings;
} Pipe;

/* The directory holding the .spv files: same as our own binary. */
static void find_spvdir(Gpu *g) {
    char path[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", path, sizeof path - 1);
    if (n <= 0) die("cannot find my own path");
    path[n] = 0;
    char *slash = strrchr(path, '/');
    if (!slash) die("own path has no directory: %s", path);
    *slash = 0;
    snprintf(g->spvdir, sizeof g->spvdir, "%s", path);
}

static void gpu_open(Gpu *g) {
    memset(g, 0, sizeof *g);
    find_spvdir(g);

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "hzgpu",
                             .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &ai };
    VK(vkCreateInstance(&ci, NULL, &g->inst));

    uint32_t count = 0;
    VK(vkEnumeratePhysicalDevices(g->inst, &count, NULL));
    if (!count) die("no vulkan device at all");
    VkPhysicalDevice *phys = zalloc(count * sizeof *phys);
    VK(vkEnumeratePhysicalDevices(g->inst, &count, phys));

    /* Pick: NVGPU_DEVICE if set, else the first real GPU.  llvmpipe
     * (PHYSICAL_DEVICE_TYPE_CPU) will not do — it is the CPU, and then we
     * measure cpu against cpu while believing we measured a gpu. */
    const char *pick = getenv("NVGPU_DEVICE");
    if (!pick) pick = getenv("NVGPU_ENHET");    /* old name, kept for scripts */
    int chosen = -1;
    for (uint32_t i = 0; i < count; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(phys[i], &p);
        if (pick) { if ((uint32_t)atoi(pick) == i) { chosen = (int)i; break; } continue; }
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { chosen = (int)i; break; }
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && chosen < 0) chosen = (int)i;
    }
    if (chosen < 0) die("found no gpu (software rendering only); set NVGPU_DEVICE to force one");
    g->phys = phys[chosen];
    free(phys);

    vkGetPhysicalDeviceProperties(g->phys, &g->props);
    vkGetPhysicalDeviceMemoryProperties(g->phys, &g->mem);

    VkPhysicalDeviceSubgroupProperties sg = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg };
    vkGetPhysicalDeviceProperties2(g->phys, &p2);
    g->subgroup = sg.subgroupSize;

    VkPhysicalDeviceFeatures feats;
    vkGetPhysicalDeviceFeatures(g->phys, &feats);
    if (!feats.shaderInt64) die("device lacks shaderInt64 — blake2b cannot run here");

    uint32_t nqueues = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nqueues, NULL);
    VkQueueFamilyProperties *qf = zalloc(nqueues * sizeof *qf);
    vkGetPhysicalDeviceQueueFamilyProperties(g->phys, &nqueues, qf);
    int family = -1;
    for (uint32_t i = 0; i < nqueues; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { family = (int)i; break; }
    free(qf);
    if (family < 0) die("device has no compute queue");
    g->queue_family = (uint32_t)family;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = g->queue_family,
                                   .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures want = { .shaderInt64 = VK_TRUE };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                              .pEnabledFeatures = &want };
    VK(vkCreateDevice(g->phys, &di, NULL, &g->dev));
    vkGetDeviceQueue(g->dev, g->queue_family, 0, &g->queue);

    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g->queue_family };
    VK(vkCreateCommandPool(g->dev, &pi, NULL, &g->pool));
}

static void gpu_close(Gpu *g) {
    vkDestroyCommandPool(g->dev, g->pool, NULL);
    vkDestroyDevice(g->dev, NULL);
    vkDestroyInstance(g->inst, NULL);
}

/* Memory type.  Always host-visible (we map everything), but beyond that it
 * depends on who reads the buffer the most:
 *
 *   want_local=1  DEVICE_LOCAL — what the GPU chews through.  On an APU it
 *                 is the same physical RAM as the CPU's, so no copy is
 *                 needed, but the memory is write-combined and therefore
 *                 uncached from the CPU side.
 *   want_local=0  HOST_CACHED — what the CPU reads back.  The bandwidth
 *                 measurement says the CPU reads the shared (wc) memory at
 *                 441 MiB/s against 30 788 MiB/s for ordinary memory: 70
 *                 times slower.  Reading home 64 MiB of matches out of wc
 *                 memory cost more than the GPU had saved by finding them,
 *                 and made the GPU path slower than its own CPU fallback.
 */
static uint32_t memtype(Gpu *g, uint32_t allowed, int want_local, int *got_local) {
    VkMemoryPropertyFlags must = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags prefer = want_local ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                              : VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    for (int round = 0; round < 2; round++) {
        VkMemoryPropertyFlags wanted = must | (round == 0 ? prefer : 0);
        for (uint32_t i = 0; i < g->mem.memoryTypeCount; i++) {
            if (!(allowed & (1u << i))) continue;
            if ((g->mem.memoryTypes[i].propertyFlags & wanted) == wanted) {
                if (got_local) *got_local = (round == 0 && want_local);
                return i;
            }
        }
    }
    die("no usable memory type (host-visible required)");
    return 0;
}

static Buf buf_create(Gpu *g, VkDeviceSize size, int device_local) {
    Buf b = { .size = size ? size : 4 };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = b.size,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK(vkCreateBuffer(g->dev, &bi, NULL, &b.buf));
    VkMemoryRequirements reqs;
    vkGetBufferMemoryRequirements(g->dev, b.buf, &reqs);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = reqs.size,
                                .memoryTypeIndex = memtype(g, reqs.memoryTypeBits, device_local, &b.device_local) };
    VK(vkAllocateMemory(g->dev, &ai, NULL, &b.mem));
    VK(vkBindBufferMemory(g->dev, b.buf, b.mem, 0));
    VK(vkMapMemory(g->dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.p));
    return b;
}

static void buf_free(Gpu *g, Buf *b) {
    if (!b->buf) return;
    vkUnmapMemory(g->dev, b->mem);
    vkDestroyBuffer(g->dev, b->buf, NULL);
    vkFreeMemory(g->dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

static uint32_t *read_spv(Gpu *g, const char *name, size_t *nbytes) {
    char path[PATH_MAX + 64];
    snprintf(path, sizeof path, "%s/%s", g->spvdir, name);
    FILE *f = fopen(path, "rb");
    if (!f) die("cannot find %s — run `make` first", path);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n % 4) die("%s is not a spir-v module (%ld bytes)", path, n);
    uint32_t *code = zalloc((size_t)n);
    if (fread(code, 1, (size_t)n, f) != (size_t)n) die("could not read %s", path);
    fclose(f);
    *nbytes = (size_t)n;
    return code;
}

static Pipe pipe_create(Gpu *g, const char *spv, uint32_t nbindings, uint32_t push_bytes,
                        uint32_t group_size) {
    Pipe r = { .nbindings = nbindings };
    VkDescriptorSetLayoutBinding *b = zalloc(nbindings * sizeof *b);
    for (uint32_t i = 0; i < nbindings; i++)
        b[i] = (VkDescriptorSetLayoutBinding){ .binding = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = nbindings, .pBindings = b };
    VK(vkCreateDescriptorSetLayout(g->dev, &li, NULL, &r.set_layout));
    free(b);

    VkPushConstantRange push = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = push_bytes };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &r.set_layout,
                                       .pushConstantRangeCount = push_bytes ? 1 : 0,
                                       .pPushConstantRanges = push_bytes ? &push : NULL };
    VK(vkCreatePipelineLayout(g->dev, &pli, NULL, &r.layout));

    size_t spvbytes = 0;
    uint32_t *code = read_spv(g, spv, &spvbytes);
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = spvbytes, .pCode = code };
    VkShaderModule module;
    VK(vkCreateShaderModule(g->dev, &smi, NULL, &module));
    free(code);

    /* The group size is a specialization constant (id 0) so the same shader
     * can run at whatever width the device actually wants. */
    VkSpecializationMapEntry entry = { .constantID = 0, .offset = 0, .size = sizeof(uint32_t) };
    VkSpecializationInfo spec = { .mapEntryCount = 1, .pMapEntries = &entry,
                                  .dataSize = sizeof(uint32_t), .pData = &group_size };
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = module, .pName = "main",
                   .pSpecializationInfo = &spec },
        .layout = r.layout };
    VK(vkCreateComputePipelines(g->dev, VK_NULL_HANDLE, 1, &ci, NULL, &r.pipe));
    vkDestroyShaderModule(g->dev, module, NULL);

    VkDescriptorPoolSize size = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = nbindings };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &size };
    VK(vkCreateDescriptorPool(g->dev, &dpi, NULL, &r.dpool));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = r.dpool, .descriptorSetCount = 1,
                                        .pSetLayouts = &r.set_layout };
    VK(vkAllocateDescriptorSets(g->dev, &dai, &r.dset));
    return r;
}

static void pipe_free(Gpu *g, Pipe *r) {
    vkDestroyDescriptorPool(g->dev, r->dpool, NULL);
    vkDestroyPipeline(g->dev, r->pipe, NULL);
    vkDestroyPipelineLayout(g->dev, r->layout, NULL);
    vkDestroyDescriptorSetLayout(g->dev, r->set_layout, NULL);
}

static void pipe_bind(Gpu *g, Pipe *r, Buf *bufs) {
    VkDescriptorBufferInfo *info = zalloc(r->nbindings * sizeof *info);
    VkWriteDescriptorSet *writes = zalloc(r->nbindings * sizeof *writes);
    for (uint32_t i = 0; i < r->nbindings; i++) {
        info[i] = (VkDescriptorBufferInfo){ .buffer = bufs[i].buf, .offset = 0, .range = VK_WHOLE_SIZE };
        writes[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->dset, .dstBinding = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &info[i] };
    }
    vkUpdateDescriptorSets(g->dev, r->nbindings, writes, 0, NULL);
    free(info); free(writes);
}

/* How many positions a matcher kernel gets per dispatch.  The driver's
 * watchdog kills a kernel that grinds too long, and the GPU shares silicon
 * with the display: a hard reset takes the desktop down with it.  256 Ki
 * positions is a few tenths of a second even on data where every chain
 * runs to the limit. */
#define SLICE (256u << 10)

/* Run one shader and wait it out.  Returns the wall time of submit+wait —
 * what a caller actually pays — not a cleaned-up kernel time. */
static double pipe_run(Gpu *g, Pipe *r, uint32_t groups, const void *push, uint32_t push_bytes) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    VK(vkAllocateCommandBuffers(g->dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->pipe);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->layout, 0, 1, &r->dset, 0, NULL);
    if (push_bytes) vkCmdPushConstants(cb, r->layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_bytes, push);
    vkCmdDispatch(cb, groups, 1, 1);
    VK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence done;
    VK(vkCreateFence(g->dev, &fi, NULL, &done));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    double t0 = now();
    VK(vkQueueSubmit(g->queue, 1, &si, done));
    VK(vkWaitForFences(g->dev, 1, &done, VK_TRUE, 120ULL * 1000 * 1000 * 1000));
    double dt = now() - t0;
    vkDestroyFence(g->dev, done, NULL);
    vkFreeCommandBuffers(g->dev, g->pool, 1, &cb);
    return dt;
}

/* ----------------------------------------------------------------------- */
/* Files and threads                                                       */
/* ----------------------------------------------------------------------- */

typedef struct { uint8_t *p; size_t n; int fd; } File;

static File file_open(const char *path) {
    File f = { .fd = -1 };
    int fd = open(path, O_RDONLY);
    if (fd < 0) return f;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode)) { close(fd); return f; }
    f.n = (size_t)st.st_size;
    if (f.n) {
        f.p = mmap(NULL, f.n, PROT_READ, MAP_PRIVATE, fd, 0);
        if (f.p == MAP_FAILED) { close(fd); f.p = NULL; return f; }
    } else {
        f.p = (uint8_t *)"";
    }
    f.fd = fd;
    return f;
}

static void file_close(File *f) {
    if (f->n && f->p) munmap(f->p, f->n);
    if (f->fd >= 0) close(f->fd);
    f->p = NULL; f->fd = -1; f->n = 0;
}

typedef struct {
    void (*fn)(void *ctx, size_t i);
    void *ctx;
    size_t n;
    size_t next;
    pthread_mutex_t lock;
} Swarm;

static void *swarm_thread(void *arg) {
    Swarm *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->lock);
        size_t i = s->next < s->n ? s->next++ : (size_t)-1;
        pthread_mutex_unlock(&s->lock);
        if (i == (size_t)-1) return NULL;
        s->fn(s->ctx, i);
    }
}

/* Run fn(ctx, 0..n-1) across `threads` threads.  Work items are handed out
 * one at a time, because file sizes are anything but evenly distributed. */
static void swarm_run(void (*fn)(void *, size_t), void *ctx, size_t n, int threads) {
    if (n == 0) return;
    if (threads < 1) threads = 1;
    if ((size_t)threads > n) threads = (int)n;
    Swarm s = { .fn = fn, .ctx = ctx, .n = n, .next = 0 };
    pthread_mutex_init(&s.lock, NULL);
    pthread_t *t = zalloc((size_t)threads * sizeof *t);
    for (int i = 0; i < threads; i++) pthread_create(&t[i], NULL, swarm_thread, &s);
    for (int i = 0; i < threads; i++) pthread_join(t[i], NULL);
    free(t);
    pthread_mutex_destroy(&s.lock);
}

/* ----------------------------------------------------------------------- */
/* LZ77 matching: buckets, CPU search                                      */
/* ----------------------------------------------------------------------- */

#define HBITS 15u
#define BUCKETS (1u << HBITS)

typedef struct {
    const uint8_t *d;
    uint32_t n;
    uint32_t *start;      /* BUCKETS + 1 */
    uint32_t *bucket;     /* positions, ascending within each bucket */
    uint32_t *loc;        /* absolute index into `bucket` for each position */
    uint32_t window, chain, nice;
    uint32_t *out;        /* len<<23 | dist */
} Match;

static inline uint32_t hash3(const uint8_t *d, uint32_t i) {
    uint32_t h = ((uint32_t)d[i] << 10) ^ ((uint32_t)d[i + 1] << 5) ^ (uint32_t)d[i + 2];
    return (uint32_t)((h * 2654435761u) >> 15) & (BUCKETS - 1u);
}

/* The buckets are built on the CPU: it is a single sweep, and the GPU is
 * supposed to measure the search, not a setup that costs the same on both
 * sides anyway. */
static void build_buckets(Match *m) {
    uint32_t n = m->n;
    uint32_t npos = n > 2 ? n - 2 : 0;
    m->start = zalloc((BUCKETS + 1) * sizeof *m->start);
    m->bucket = zalloc((size_t)(npos ? npos : 1) * sizeof *m->bucket);
    m->loc = zalloc((size_t)(n ? n : 1) * sizeof *m->loc);
    for (uint32_t i = 0; i < npos; i++) m->start[hash3(m->d, i) + 1]++;
    for (uint32_t h = 0; h < BUCKETS; h++) m->start[h + 1] += m->start[h];
    uint32_t *cursor = zalloc(BUCKETS * sizeof *cursor);
    for (uint32_t h = 0; h < BUCKETS; h++) cursor[h] = m->start[h];
    for (uint32_t i = 0; i < npos; i++) {
        uint32_t h = hash3(m->d, i);
        uint32_t idx = cursor[h]++;
        m->bucket[idx] = i;
        m->loc[i] = idx;
    }
    free(cursor);
}

static void match_part(void *ctx, size_t piece) {
    Match *m = ctx;
    uint32_t per = 1u << 16;
    uint32_t from = (uint32_t)piece * per;
    uint32_t to = from + per < m->n ? from + per : m->n;
    for (uint32_t pos = from; pos < to; pos++) {
        if (pos + 2 >= m->n) { m->out[pos] = 0; continue; }
        uint32_t cap = m->n - pos < 258 ? m->n - pos : 258;
        uint32_t h = hash3(m->d, pos);
        uint32_t i = m->loc[pos], base = m->start[h];
        uint32_t lower = pos > m->window ? pos - m->window : 0;
        uint32_t best_len = 0, best_dist = 0, tried = 0;
        while (i > base && tried < m->chain) {
            i--;
            uint32_t c = m->bucket[i];
            if (c < lower) break;
            tried++;
            if (best_len >= 1 && m->d[c + best_len] != m->d[pos + best_len]) continue;
            uint32_t l = 0;
            while (l < cap && m->d[c + l] == m->d[pos + l]) l++;
            if (l > best_len) {
                best_len = l; best_dist = pos - c;
                if (l >= cap || l >= m->nice) break;      /* nice is enough */
            }
        }
        m->out[pos] = best_len >= 3 ? (best_len << 23) | best_dist : 0;
    }
}

static double match_cpu(Match *m, int threads) {
    size_t pieces = ((size_t)m->n + (1u << 16) - 1) / (1u << 16);
    double t0 = now();
    swarm_run(match_part, m, pieces ? pieces : 1, threads);
    return now() - t0;
}

/* ----------------------------------------------------------------------- */
/* Commands                                                                */
/* ----------------------------------------------------------------------- */

static const char *memname(VkMemoryPropertyFlags f) {
    static char b[128];
    snprintf(b, sizeof b, "%s%s%s%s",
             (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "device " : "",
             (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "visible " : "",
             (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "coherent " : "",
             (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "cached" : "");
    return b;
}

static int cmd_device(Gpu *g) {
    say("device: %s\n", g->props.deviceName);
    const char *type = g->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrated"
                     : g->props.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "discrete"
                     : "other";
    say("  type              %s\n", type);
    say("  driver            vulkan %u.%u.%u\n",
        VK_VERSION_MAJOR(g->props.apiVersion), VK_VERSION_MINOR(g->props.apiVersion),
        VK_VERSION_PATCH(g->props.apiVersion));
    say("  subgroup          %u threads\n", g->subgroup);
    say("  workgroup max     %u threads, %u bytes shared memory\n",
        g->props.limits.maxComputeWorkGroupInvocations,
        g->props.limits.maxComputeSharedMemorySize);
    say("  buffer max        %.1f GiB\n", g->props.limits.maxStorageBufferRange / 1073741824.0);
    for (uint32_t i = 0; i < g->mem.memoryHeapCount; i++)
        say("  heap %u            %.1f GiB%s\n", i, g->mem.memoryHeaps[i].size / 1073741824.0,
            (g->mem.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (device-local)" : "");
    Buf b = buf_create(g, 1 << 20, 1);
    say("  zero-copy         %s\n", b.device_local
        ? "yes — device-local memory is mappable (shared RAM)" : "no — must copy");
    for (uint32_t i = 0; i < g->mem.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = g->mem.memoryTypes[i].propertyFlags;
        if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            say("  memory type %u     %s\n", i, memname(f));
            break;
        }
    }
    buf_free(g, &b);
    return 0;
}

typedef struct { const uint32_t *d; uint32_t words; uint64_t *sums; } Sum;

static void sum_part(void *ctx, size_t i) {
    Sum *l = ctx;
    uint32_t per = 1u << 20;
    uint32_t from = (uint32_t)i * per;
    uint32_t to = from + per < l->words ? from + per : l->words;
    uint64_t s = 0;
    for (uint32_t k = from; k < to; k++) s += l->d[k];
    l->sums[i] = s;
}

static int cmd_band(Gpu *g, double mib, int threads) {
    uint32_t words = (uint32_t)(mib * 1048576.0 / 4.0);
    Buf data = buf_create(g, (VkDeviceSize)words * 4, 1);
    uint32_t *d = data.p;
    for (uint32_t i = 0; i < words; i++) d[i] = i * 2654435761u;

    uint32_t groups = 512, group = 64;
    Buf out = buf_create(g, (VkDeviceSize)groups * group * 4, 1);
    Pipe r = pipe_create(g, "band.spv", 2, 8, group);
    Buf bufs[2] = { data, out };
    pipe_bind(g, &r, bufs);
    struct { uint32_t words, rounds; } push = { words, 1 };

    pipe_run(g, &r, groups, &push, sizeof push);          /* warm up */
    double best = 1e30;
    for (int i = 0; i < 3; i++) {
        double dt = pipe_run(g, &r, groups, &push, sizeof push);
        if (dt < best) best = dt;
    }
    double gpu = mib / best;

    /* The CPU side reads ordinary memory.  Letting it read the GPU buffer
     * would measure the wrong thing: the shared memory is write-combined,
     * i.e. uncached from the CPU side, and a CPU reading uncached memory is
     * on its knees.  That is the price of "zero-copy" on an APU, and it is
     * measured separately below. */
    size_t pieces = ((size_t)words + (1u << 20) - 1) / (1u << 20);
    uint32_t *plain = zalloc((size_t)words * 4);
    memcpy(plain, d, (size_t)words * 4);
    Sum l = { .d = plain, .words = words, .sums = zalloc(pieces * sizeof(uint64_t)) };
    double cbest = 1e30;
    for (int i = 0; i < 3; i++) {
        double t0 = now();
        swarm_run(sum_part, &l, pieces, threads);
        double dt = now() - t0;
        if (dt < cbest) cbest = dt;
    }
    double cpu = mib / cbest;

    Sum l2 = { .d = d, .words = words, .sums = l.sums };
    double wbest = 1e30;
    for (int i = 0; i < 2; i++) {
        double t0 = now();
        swarm_run(sum_part, &l2, pieces, threads);
        double dt = now() - t0;
        if (dt < wbest) wbest = dt;
    }
    double wc = mib / wbest;
    free(l.sums); free(plain);

    say("band: %.0f MiB read\n", mib);
    say("  gpu reads shared memory     %7.3f s  %8.0f MiB/s\n", best, gpu);
    say("  cpu reads plain memory      %7.3f s  %8.0f MiB/s  (%d threads)\n", cbest, cpu, threads);
    say("  cpu reads shared memory     %7.3f s  %8.0f MiB/s  (uncached/wc — %.0fx penalty)\n",
        wbest, wc, cpu / wc);
    say("  gpu/cpu %.2fx\n", gpu / cpu);
    pipe_free(g, &r);
    buf_free(g, &out);
    buf_free(g, &data);
    return 0;
}

/* --- hash ---------------------------------------------------------------- */

typedef struct {
    char **paths;
    size_t count;
    uint8_t *hashes;      /* 16 bytes per file */
    uint64_t *sizes;
    int *skipped;         /* 1 = too big for the gpu, done on cpu */
} HashJob;

static void hash_cpu_one(void *ctx, size_t i) {
    HashJob *h = ctx;
    File f = file_open(h->paths[i]);
    if (f.fd < 0 && f.n == 0 && !f.p) { memset(&h->hashes[i * 16], 0, 16); h->sizes[i] = 0; return; }
    blake2b128(f.p, f.n, &h->hashes[i * 16]);
    h->sizes[i] = f.n;
    file_close(&f);
}

static void print_hashes(HashJob *h) {
    for (size_t i = 0; i < h->count; i++) {
        char hex[33];
        for (int k = 0; k < 16; k++) snprintf(hex + k * 2, 3, "%02x", h->hashes[i * 16 + k]);
        printf("%s  %s\n", hex, h->paths[i]);
    }
}

/* The GPU path: files are copied into a shared buffer in batches, one
 * kernel thread per file.  The copy is counted in the time — it is what
 * the GPU path actually costs a caller, and leaving it out would measure
 * the wrong thing.
 *
 * The gather is threaded.  The first version read the files in one thread
 * and compared against a CPU path that had 24: that gave the GPU 6 MiB/s
 * against the CPU's 693 and was a measure of my own code, not of the
 * hardware.  The kernel took 0.038 s of the 8.8 seconds.
 */

typedef struct {
    HashJob *h;
    uint8_t *dp;
    size_t *fileno;       /* job -> file number */
    uint32_t *jp;         /* (word offset, length) per job */
    size_t count;
} Gather;

static void gather_one(void *ctx, size_t k) {
    Gather *in = ctx;
    size_t i = in->fileno[k];
    size_t off = (size_t)in->jp[k * 2] * 4, n = in->jp[k * 2 + 1];
    File f = file_open(in->h->paths[i]);
    if (!f.p) { in->h->skipped[i] = 1; return; }      /* vanished under us: cpu takes it */
    size_t m = f.n < n ? f.n : n;
    if (m) memcpy(in->dp + off, f.p, m);
    if (m < n) memset(in->dp + off + m, 0, n - m);
    size_t pad = (4 - (n & 3)) & 3;
    if (pad) memset(in->dp + off + n, 0, pad);
    file_close(&f);
}

static void size_one(void *ctx, size_t i) {
    HashJob *h = ctx;
    struct stat st;
    if (stat(h->paths[i], &st) || !S_ISREG(st.st_mode)) { h->skipped[i] = 1; h->sizes[i] = 0; return; }
    h->sizes[i] = (uint64_t)st.st_size;
}

static double hash_gpu(Gpu *g, HashJob *h, size_t batch_bytes, size_t bigfile,
                       size_t *gpu_bytes, size_t *gpu_files, double *kernel_time, int threads) {
    uint32_t group = g->subgroup < 64 ? g->subgroup : 64;
    size_t max_jobs = 1u << 16;
    Buf data = buf_create(g, batch_bytes + 1024, 1);
    Buf jobs = buf_create(g, max_jobs * 8, 1);
    Buf out  = buf_create(g, max_jobs * 16, 0);         /* cpu reads it back: cached */
    Pipe r = pipe_create(g, "blake2b.spv", 3, 4, group);
    Buf bufs[3] = { data, jobs, out };
    pipe_bind(g, &r, bufs);

    uint8_t *dp = data.p;
    uint32_t *jp = jobs.p;
    uint8_t *up = out.p;
    size_t *fileno = zalloc(max_jobs * sizeof *fileno);

    double t0 = now();
    *kernel_time = 0; *gpu_bytes = 0; *gpu_files = 0;

    /* Sizes first, in parallel: they decide how the batches can be packed. */
    swarm_run(size_one, h, h->count, threads);

    size_t i = 0;
    while (i < h->count) {
        size_t bytes = 0, count = 0;
        while (i < h->count && count < max_jobs) {
            if (h->skipped[i]) { i++; continue; }
            size_t n = (size_t)h->sizes[i];
            if (n > bigfile) { h->skipped[i] = 1; i++; continue; }   /* one thread per file: too big */
            size_t off = (bytes + 3) & ~(size_t)3;
            if (off + n + 4 > batch_bytes) break;
            jp[count * 2]     = (uint32_t)(off / 4);
            jp[count * 2 + 1] = (uint32_t)n;
            fileno[count] = i;
            bytes = off + n + ((4 - (n & 3)) & 3);
            count++;
            i++;
        }
        if (!count) continue;
        Gather in = { .h = h, .dp = dp, .fileno = fileno, .jp = jp, .count = count };
        swarm_run(gather_one, &in, count, threads);
        uint32_t push = (uint32_t)count;
        uint32_t groups = (uint32_t)((count + group - 1) / group);
        *kernel_time += pipe_run(g, &r, groups, &push, sizeof push);
        for (size_t k = 0; k < count; k++) {
            size_t f = fileno[k];
            if (!h->skipped[f]) memcpy(&h->hashes[f * 16], up + k * 16, 16);
        }
        *gpu_bytes += bytes;
        *gpu_files += count;
    }
    double dt = now() - t0;

    /* The skipped files are done on the CPU — the answer would otherwise be
     * incomplete. */
    size_t left = 0;
    for (size_t k = 0; k < h->count; k++) if (h->skipped[k]) left++;
    if (left) {
        HashJob sub = { .paths = zalloc(left * sizeof(char *)),
                        .hashes = zalloc(left * 16), .sizes = zalloc(left * 8),
                        .skipped = zalloc(left * sizeof(int)), .count = left };
        size_t m = 0;
        for (size_t k = 0; k < h->count; k++) if (h->skipped[k]) sub.paths[m++] = h->paths[k];
        swarm_run(hash_cpu_one, &sub, left, threads);
        m = 0;
        for (size_t k = 0; k < h->count; k++)
            if (h->skipped[k]) { memcpy(&h->hashes[k * 16], &sub.hashes[m * 16], 16);
                                 h->sizes[k] = sub.sizes[m]; m++; }
        free(sub.paths); free(sub.hashes); free(sub.sizes); free(sub.skipped);
    }

    free(fileno);
    pipe_free(g, &r);
    buf_free(g, &out); buf_free(g, &jobs); buf_free(g, &data);
    return dt;
}

static HashJob read_paths(void) {
    HashJob h = { 0 };
    size_t cap = 1024;
    h.paths = zalloc(cap * sizeof(char *));
    char *line = NULL; size_t lcap = 0; ssize_t n;
    while ((n = getline(&line, &lcap, stdin)) > 0) {
        while (n && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;
        if (!n) continue;
        if (h.count == cap) { cap *= 2; h.paths = realloc(h.paths, cap * sizeof(char *));
                              if (!h.paths) die("out of memory"); }
        h.paths[h.count++] = strdup(line);
    }
    free(line);
    h.hashes = zalloc(h.count * 16 + 16);
    h.sizes = zalloc(h.count * 8 + 8);
    h.skipped = zalloc(h.count * sizeof(int) + 4);
    return h;
}

static int cmd_hash(Gpu *g, int cpu_only, int threads, size_t batch, size_t bigfile) {
    HashJob h = read_paths();
    if (!h.count) die("no paths on stdin");
    double dt, kernel = 0;
    size_t gbytes = 0, gfiles = 0;
    if (cpu_only) {
        double t0 = now();
        swarm_run(hash_cpu_one, &h, h.count, threads);
        dt = now() - t0;
    } else {
        dt = hash_gpu(g, &h, batch, bigfile, &gbytes, &gfiles, &kernel, threads);
    }
    uint64_t total = 0;
    for (size_t i = 0; i < h.count; i++) total += h.sizes[i];
    print_hashes(&h);
    fprintf(stderr, "hash %s: %zu files, %.1f MiB in %.3f s = %.0f MiB/s",
            cpu_only ? "cpu" : "gpu", h.count, total / 1048576.0, dt, total / 1048576.0 / dt);
    if (!cpu_only) {
        size_t cpu_files = h.count - gfiles;
        fprintf(stderr, " (kernel %.3f s, %zu files on gpu, %zu too big and done on cpu)",
                kernel, gfiles, cpu_files);
    } else {
        fprintf(stderr, " (%d threads)", threads);
    }
    fputc('\n', stderr);
    return 0;
}

/* --- matcher ------------------------------------------------------------- */

typedef struct { double build, cpu, gpu, copy; uint32_t mismatches; uint32_t n; } MatchTime;

static MatchTime matcher_vs_cpu(Gpu *g, const uint8_t *data, uint32_t n,
                                uint32_t window, uint32_t chain, uint32_t nice,
                                int threads, int cpu_only) {
    MatchTime r = { .n = n };
    Match m = { .d = data, .n = n, .window = window, .chain = chain, .nice = nice };
    double t0 = now();
    build_buckets(&m);
    r.build = now() - t0;

    uint32_t *out_cpu = zalloc((size_t)n * 4 + 4);
    m.out = out_cpu;
    r.cpu = match_cpu(&m, threads);

    if (!cpu_only) {
        uint32_t npos = n > 2 ? n - 2 : 1;
        t0 = now();
        Buf bdata  = buf_create(g, ((size_t)n + 4 + 3) & ~(size_t)3, 1);
        Buf bstart = buf_create(g, (BUCKETS + 1) * 4, 1);
        Buf bbucket = buf_create(g, (size_t)npos * 4, 1);
        Buf bloc   = buf_create(g, (size_t)n * 4 + 4, 1);
        Buf bout   = buf_create(g, (size_t)n * 4 + 4, 0);  /* cpu reads it back: cached */
        memcpy(bdata.p, data, n);
        memset((uint8_t *)bdata.p + n, 0, 4);
        memcpy(bstart.p, m.start, (BUCKETS + 1) * 4);
        memcpy(bbucket.p, m.bucket, (size_t)(n > 2 ? n - 2 : 0) * 4);
        memcpy(bloc.p, m.loc, (size_t)n * 4);
        r.copy = now() - t0;

        uint32_t group = g->subgroup < 64 ? g->subgroup : 64;
        Pipe pipe = pipe_create(g, "matcher.spv", 5, 24, group);
        Buf bufs[5] = { bdata, bstart, bbucket, bloc, bout };
        pipe_bind(g, &pipe, bufs);
        struct { uint32_t n, window, chain, hmask, base, nice; } push =
            { n, window, chain, BUCKETS - 1, 0, nice };
        for (uint32_t base = 0; base < n; base += SLICE) {
            uint32_t m2 = n - base < SLICE ? n - base : SLICE;
            push.base = base;
            r.gpu += pipe_run(g, &pipe, (m2 + group - 1) / group, &push, sizeof push);
        }

        const uint32_t *out_gpu = bout.p;
        for (uint32_t i = 0; i < n; i++) if (out_gpu[i] != out_cpu[i]) r.mismatches++;

        pipe_free(g, &pipe);
        buf_free(g, &bout); buf_free(g, &bloc); buf_free(g, &bbucket);
        buf_free(g, &bstart); buf_free(g, &bdata);
    }

    free(out_cpu); free(m.start); free(m.bucket); free(m.loc);
    return r;
}

static int cmd_matcher(Gpu *g, const char *path, uint32_t window, uint32_t chain,
                       uint32_t nice, int threads, int cpu_only, double cap_mib) {
    File f = file_open(path);
    if (!f.p) die("cannot read %s", path);
    uint32_t n = (uint32_t)(f.n < (size_t)(cap_mib * 1048576.0) ? f.n : (size_t)(cap_mib * 1048576.0));
    if (n < 16) die("%s is too small (%u bytes)", path, n);

    MatchTime r = matcher_vs_cpu(g, f.p, n, window, chain, nice, threads, cpu_only);
    double mib = n / 1048576.0;
    say("matcher: %s, %.1f MiB, window %u, chain limit %u, nice %u\n", path, mib, window, chain, nice);
    say("  bucket build (cpu, both paths need it)     %7.3f s  %8.0f MiB/s\n", r.build, mib / r.build);
    say("  cpu search (%2d threads)                    %7.3f s  %8.0f MiB/s\n", threads, r.cpu, mib / r.cpu);
    if (!cpu_only) {
        say("  gpu copy in                                %7.3f s  %8.0f MiB/s\n", r.copy, mib / r.copy);
        say("  gpu search                                 %7.3f s  %8.0f MiB/s\n", r.gpu, mib / r.gpu);
        say("  gpu/cpu search only    %.2fx\n", r.cpu / r.gpu);
        say("  gpu/cpu with copy      %.2fx\n", r.cpu / (r.gpu + r.copy));
        say("  mismatches against cpu %u of %u positions%s\n", r.mismatches, r.n,
            r.mismatches ? "  — wrong, the timings mean nothing" : "  (identical)");
    }
    file_close(&f);
    return r.mismatches ? 1 : 0;
}

/* --- selftest ------------------------------------------------------------ */

static int hex_eq(const uint8_t h[16], const char *expect) {
    char hex[33];
    for (int k = 0; k < 16; k++) snprintf(hex + k * 2, 3, "%02x", h[k]);
    return strcmp(hex, expect) == 0;
}

/* blake2b-128 of known inputs, taken from hashlib (the python side's own
 * implementation).  If these do not match, the whole GPU mode is pointless,
 * because it is not the hash the archives were built with. */
static const struct { size_t n; const char *pattern; const char *expect; } VECTORS[] = {
    { 0,    "",    "cae66941d9efbd404e4d88758ea67670" },
    { 3,    "abc", "cf4ab791c62b8d2b2109c90275287816" },
    { 128,  "a",   "1271d28e731afb57bdfba6bdaf3501be" },
    { 129,  "a",   "68b7bafbad5055c4ebe6eb170a0f8524" },
};

static int cmd_selftest(Gpu *g, int threads) {
    int fails = 0;
    say("selftest: blake2b-128 on cpu against known values\n");
    for (size_t i = 0; i < sizeof VECTORS / sizeof *VECTORS; i++) {
        uint8_t *b = zalloc(VECTORS[i].n + 1);
        size_t ml = strlen(VECTORS[i].pattern);
        for (size_t k = 0; k < VECTORS[i].n; k++) b[k] = (uint8_t)VECTORS[i].pattern[ml ? k % ml : 0];
        uint8_t h[16];
        blake2b128(b, VECTORS[i].n, h);
        int ok = hex_eq(h, VECTORS[i].expect);
        say("  %4zu bytes  %s\n", VECTORS[i].n, ok ? "ok" : "fail");
        if (!ok) fails++;
        free(b);
    }
    /* 256 bytes 0..255 repeated four times — covers every byte value. */
    {
        uint8_t b[1024];
        for (int i = 0; i < 1024; i++) b[i] = (uint8_t)(i & 255);
        uint8_t h[16];
        blake2b128(b, 1024, h);
        int ok = hex_eq(h, "0b4661484ed1cc8e28c4a7f18e8b1019");
        say("  1024 bytes  %s\n", ok ? "ok" : "fail");
        if (!ok) fails++;
    }

    /* GPU against CPU across the troublesome lengths: empty, word
     * boundaries, the 128 boundary (blake2b's last block must never be
     * empty unless the message is) and a couple of big ones. */
    say("selftest: blake2b-128 gpu against cpu across %d lengths\n", 23);
    static const uint32_t lengths[] = { 0,1,2,3,4,5,7,8,63,64,65,127,128,129,
                                        255,256,257,1000,4096,65536,131071,131072,1048577 };
    size_t count = sizeof lengths / sizeof *lengths;
    uint32_t group = g->subgroup < 64 ? g->subgroup : 64;
    size_t total = 0;
    for (size_t i = 0; i < count; i++) total += (lengths[i] + 3) & ~3u;
    Buf data = buf_create(g, total + 1024, 1);
    Buf jobs = buf_create(g, count * 8, 1);
    Buf out  = buf_create(g, count * 16, 1);
    Pipe r = pipe_create(g, "blake2b.spv", 3, 4, group);
    Buf bufs[3] = { data, jobs, out };
    pipe_bind(g, &r, bufs);
    uint8_t *dp = data.p; uint32_t *jp = jobs.p;
    size_t off = 0;
    uint8_t **copies = zalloc(count * sizeof(uint8_t *));
    for (size_t i = 0; i < count; i++) {
        uint32_t n = lengths[i];
        copies[i] = zalloc(n + 1);
        for (uint32_t k = 0; k < n; k++) copies[i][k] = (uint8_t)((k * 37 + i * 11) & 255);
        memcpy(dp + off, copies[i], n);
        size_t pad = (4 - (n & 3)) & 3;
        if (pad) memset(dp + off + n, 0, pad);
        jp[i * 2] = (uint32_t)(off / 4);
        jp[i * 2 + 1] = n;
        off += n + pad;
    }
    uint32_t push = (uint32_t)count;
    pipe_run(g, &r, (uint32_t)((count + group - 1) / group), &push, sizeof push);
    const uint8_t *up = out.p;
    for (size_t i = 0; i < count; i++) {
        uint8_t h[16];
        blake2b128(copies[i], lengths[i], h);
        int ok = memcmp(h, up + i * 16, 16) == 0;
        if (!ok) {
            fails++;
            say("  %8u bytes  fail\n", lengths[i]);
        }
        free(copies[i]);
    }
    free(copies);
    say("  %s\n", fails ? "mismatches found" : "all identical to cpu");
    pipe_free(g, &r);
    buf_free(g, &out); buf_free(g, &jobs); buf_free(g, &data);

    /* The matcher: gpu against cpu on data with both repeats and noise. */
    say("selftest: lz77 matching gpu against cpu\n");
    uint32_t n = 4u << 20;
    uint8_t *b = zalloc(n);
    const char *snippet = "void main(void) { return nvpack(0x1234, \"context\"); }\n";
    size_t bl = strlen(snippet);
    uint32_t seed = 12345;
    for (uint32_t i = 0; i < n; ) {
        seed = seed * 1103515245u + 12345u;
        if ((seed >> 16) % 3) {                      /* two thirds text */
            size_t t = bl < n - i ? bl : n - i;
            memcpy(b + i, snippet, t); i += (uint32_t)t;
        } else {                                      /* one third noise */
            uint32_t t = (seed >> 8) % 64 + 1;
            for (uint32_t k = 0; k < t && i < n; k++, i++) {
                seed = seed * 1103515245u + 12345u;
                b[i] = (uint8_t)(seed >> 24);
            }
        }
    }
    for (int round = 0; round < 2; round++) {
        uint32_t chain = round ? 4096 : 32;
        MatchTime r2 = matcher_vs_cpu(g, b, n, 32768, chain, 258, threads, 0);
        say("  chain limit %-5u mismatches %u of %u  (cpu %.3f s, gpu %.3f s)\n",
            chain, r2.mismatches, r2.n, r2.cpu, r2.gpu);
        if (r2.mismatches) fails++;
    }
    free(b);

    say("%s\n", fails ? "selftest failed" : "all tests passed");
    return fails ? 1 : 0;
}

/* ----------------------------------------------------------------------- */
/* nvlz — the codec that actually uses the gpu                             */
/* ----------------------------------------------------------------------- */
/* The match search is the only broadly parallel part of a packer, and the
 * measurement above says the GPU does it four to six times faster than 24
 * CPU threads.  But zlib, xz and zstd cannot accept precomputed matches —
 * their output must be bit for bit what their own searches would have
 * produced.  Hence this codec: nvpack's own format, where the expensive
 * half (the search) runs on the GPU and the serial half (token choice and
 * entropy coding) on the CPU.
 *
 * Unpacking is pure CPU.  An archive packed with nvlz must open on a
 * machine without Vulkan, or the archive is a hostage of a driver.
 *
 * The format: "NVLZ1", then chunks of at most CHUNK bytes of raw data.
 * Each chunk carries raw length, coded length and a raw-storage flag (for
 * when coding gained nothing).  Matches only point within their own chunk
 * — that costs a few tenths of a percent at chunk boundaries and makes the
 * unpacker simple enough to trust.
 */

#define CHUNK    (32u << 20)
#define LZ_WINDOW (1u << 22)          /* 4 MiB back; the distance fits in 23 bits */
#define PROB_BIT 11
#define PROB_ONE (1u << PROB_BIT)
#define PROB_SHIFT 5

typedef uint16_t Prob;

/* The models.  Three choices the measurement drove out (they took 2.1x ->
 * 4.3x on the code corpus):
 *
 *   literal[previous byte][...]  — order-1.  Sixteen contexts (the upper
 *     nibble) were nowhere near enough: in text the next character is
 *     decided by the one before, and 'q' followed by 'u' is not the same
 *     distribution as ' ' followed by 'u'.
 *   rep — "same distance as the previous match".  In structured text
 *     (json, logs, source code with repeated lines) the most common
 *     distance is the one just used, and then one bit is cheaper than a
 *     distance code.
 *   length[is rep][...] — rep matches have a different length distribution
 *     than fresh matches, so they get their own trees. */
typedef struct {
    Prob match[2][16];
    Prob rep[2][16];
    Prob literal[256][256];
    Prob length[2][256];
    Prob slot[32];
    Prob mid[24][256];
} Models;

static void models_init(Models *m) {
    Prob *p = (Prob *)m;
    size_t n = sizeof *m / sizeof(Prob);
    for (size_t i = 0; i < n; i++) p[i] = PROB_ONE / 2;
}

/* --- the range coder (lzma flavour) -------------------------------------- */

typedef struct {
    uint8_t *out; size_t n, cap;
    uint64_t low; uint32_t range; uint8_t cache; uint64_t cache_count;
} Enc;

static void enc_byte(Enc *k, uint8_t b) {
    if (k->n == k->cap) {
        k->cap = k->cap ? k->cap * 2 : 1 << 16;
        k->out = realloc(k->out, k->cap);
        if (!k->out) die("out of memory in the encoder");
    }
    k->out[k->n++] = b;
}

static void enc_init(Enc *k) {
    k->out = NULL; k->n = 0; k->cap = 0;
    k->low = 0; k->range = 0xFFFFFFFFu; k->cache = 0; k->cache_count = 1;
}

static void enc_shift(Enc *k) {
    if ((uint32_t)(k->low >> 32) != 0 || (uint32_t)k->low < 0xFF000000u) {
        uint8_t t = k->cache;
        do { enc_byte(k, (uint8_t)(t + (uint8_t)(k->low >> 32))); t = 0xFF; } while (--k->cache_count);
        k->cache = (uint8_t)((uint32_t)k->low >> 24);
    }
    k->cache_count++;
    k->low = (uint64_t)((uint32_t)k->low << 8);
}

static void enc_bit(Enc *k, Prob *p, int bit) {
    uint32_t bound = (k->range >> PROB_BIT) * *p;
    if (!bit) { k->range = bound; *p = (Prob)(*p + ((PROB_ONE - *p) >> PROB_SHIFT)); }
    else { k->low += bound; k->range -= bound; *p = (Prob)(*p - (*p >> PROB_SHIFT)); }
    while (k->range < (1u << 24)) { k->range <<= 8; enc_shift(k); }
}

static void enc_raw(Enc *k, uint32_t v, int bits) {
    for (int i = bits - 1; i >= 0; i--) {
        k->range >>= 1;
        if ((v >> i) & 1) k->low += k->range;
        while (k->range < (1u << 24)) { k->range <<= 8; enc_shift(k); }
    }
}

static void enc_flush(Enc *k) { for (int i = 0; i < 5; i++) enc_shift(k); }

/* Bit tree: `bits` bits, one model per node.  Same order in encoder and
 * decoder — that is the whole contract between them. */
static void enc_tree(Enc *k, Prob *tree, uint32_t v, int bits) {
    uint32_t node = 1;
    for (int i = bits - 1; i >= 0; i--) {
        int bit = (v >> i) & 1;
        enc_bit(k, &tree[node], bit);
        node = (node << 1) | (uint32_t)bit;
    }
}

typedef struct {
    const uint8_t *in; size_t n, i;
    uint32_t range, code;
} Dec;

static uint8_t dec_byte(Dec *a) { return a->i < a->n ? a->in[a->i++] : 0; }

static void dec_init(Dec *a, const uint8_t *in, size_t n) {
    a->in = in; a->n = n; a->i = 0;
    a->range = 0xFFFFFFFFu; a->code = 0;
    dec_byte(a);                                    /* the encoder's first byte is always 0 */
    for (int i = 0; i < 4; i++) a->code = (a->code << 8) | dec_byte(a);
}

static int dec_bit(Dec *a, Prob *p) {
    uint32_t bound = (a->range >> PROB_BIT) * *p;
    int bit;
    if (a->code < bound) { a->range = bound; *p = (Prob)(*p + ((PROB_ONE - *p) >> PROB_SHIFT)); bit = 0; }
    else { a->code -= bound; a->range -= bound; *p = (Prob)(*p - (*p >> PROB_SHIFT)); bit = 1; }
    while (a->range < (1u << 24)) { a->range <<= 8; a->code = (a->code << 8) | dec_byte(a); }
    return bit;
}

static uint32_t dec_raw(Dec *a, int bits) {
    uint32_t v = 0;
    for (int i = 0; i < bits; i++) {
        a->range >>= 1;
        int bit = 0;
        if (a->code >= a->range) { a->code -= a->range; bit = 1; }
        while (a->range < (1u << 24)) { a->range <<= 8; a->code = (a->code << 8) | dec_byte(a); }
        v = (v << 1) | (uint32_t)bit;
    }
    return v;
}

static uint32_t dec_tree(Dec *a, Prob *tree, int bits) {
    uint32_t node = 1;
    for (int i = 0; i < bits; i++) node = (node << 1) | (uint32_t)dec_bit(a, &tree[node]);
    return node - (1u << bits);
}

static int bitlen(uint32_t v) { int n = 0; while (v) { n++; v >>= 1; } return n; }

/* --- coding one chunk ----------------------------------------------------- */

/* The distance: first how many bits it has (slot), then the bits below the
 * leading one.  The top eight of those get models — that is where the
 * pattern sits, because distances in real data cluster.  The rest is coded
 * raw. */
static void enc_dist(Enc *k, Models *m, uint32_t dist) {
    uint32_t v = dist - 1;
    int nb = bitlen(v);
    enc_tree(k, m->slot, (uint32_t)nb, 5);
    if (nb >= 2) {
        int rest = nb - 1;
        uint32_t below = v & ((1u << rest) - 1);
        if (rest <= 8) enc_tree(k, m->mid[nb], below, rest);
        else {
            enc_tree(k, m->mid[nb], below >> (rest - 8), 8);
            enc_raw(k, below & ((1u << (rest - 8)) - 1), rest - 8);
        }
    }
}

static uint32_t dec_dist(Dec *a, Models *m) {
    int nb = (int)dec_tree(a, m->slot, 5);
    uint32_t v;
    if (nb == 0) v = 0;
    else if (nb == 1) v = 1;
    else {
        int rest = nb - 1;
        uint32_t below;
        if (rest <= 8) below = dec_tree(a, m->mid[nb], rest);
        else {
            below = dec_tree(a, m->mid[nb], 8) << (rest - 8);
            below |= dec_raw(a, rest - 8);
        }
        v = (1u << rest) | below;
    }
    return v + 1;
}

/* The lazy choice: a match immediately followed by a longer one is not
 * worth taking.  It is zlib's `max_lazy` idea, and it gives a couple of
 * percent for free. */
static size_t encode_chunk(const uint8_t *d, uint32_t n, const uint32_t *mt, Enc *k) {
    Models *m = zalloc(sizeof *m);
    models_init(m);
    enc_init(k);
    uint32_t pos = 0;
    uint8_t prev = 0;
    int prev_was_match = 0;
    uint32_t rep0 = 0;
    while (pos < n) {
        uint32_t a = mt[pos];
        uint32_t len = a >> 23, dist = a & 0x7FFFFFu;
        if (len >= 3 && pos + 1 < n) {
            uint32_t b = mt[pos + 1];
            if ((b >> 23) > len) len = 0;          /* the next position takes home more */
        }
        Prob *pm = &m->match[prev_was_match][prev >> 4];
        if (len >= 3) {
            enc_bit(k, pm, 1);
            int is_rep = (rep0 && dist == rep0);
            enc_bit(k, &m->rep[prev_was_match][prev >> 4], is_rep);
            enc_tree(k, m->length[is_rep], len - 3, 8);
            if (!is_rep) { enc_dist(k, m, dist); rep0 = dist; }
            pos += len;
            prev = d[pos - 1];
            prev_was_match = 1;
        } else {
            enc_bit(k, pm, 0);
            enc_tree(k, m->literal[prev], d[pos], 8);
            prev = d[pos];
            pos++;
            prev_was_match = 0;
        }
    }
    enc_flush(k);
    free(m);
    return k->n;
}

static int decode_chunk(const uint8_t *in, size_t inn, uint8_t *out, uint32_t n) {
    Models *m = zalloc(sizeof *m);
    models_init(m);
    Dec a;
    dec_init(&a, in, inn);
    uint32_t pos = 0;
    uint8_t prev = 0;
    int prev_was_match = 0;
    uint32_t rep0 = 0;
    while (pos < n) {
        Prob *pm = &m->match[prev_was_match][prev >> 4];
        if (dec_bit(&a, pm)) {
            int is_rep = dec_bit(&a, &m->rep[prev_was_match][prev >> 4]);
            uint32_t len = dec_tree(&a, m->length[is_rep], 8) + 3;
            uint32_t dist;
            if (is_rep) dist = rep0;
            else { dist = dec_dist(&a, m); rep0 = dist; }
            if (!dist || dist > pos || len > n - pos) { free(m); return -1; }   /* corrupt stream */
            for (uint32_t i = 0; i < len; i++) out[pos + i] = out[pos + i - dist];
            pos += len;
            prev = out[pos - 1];
            prev_was_match = 1;
        } else {
            uint8_t b = (uint8_t)dec_tree(&a, m->literal[prev], 8);
            out[pos++] = b;
            prev = b;
            prev_was_match = 0;
        }
    }
    free(m);
    return 0;
}

/* --- matches for one chunk: gpu if present, else cpu ---------------------- */

typedef struct {
    Gpu *g;
    Pipe pipe;
    Buf data, start, bucket, loc, out;
    uint32_t cap;            /* the chunk's maximum size */
    uint32_t group;
    int ready;
} Matcher;

static void matcher_open(Matcher *mm, Gpu *g, uint32_t cap) {
    memset(mm, 0, sizeof *mm);
    mm->g = g; mm->cap = cap;
    if (!g) return;
    mm->group = g->subgroup < 64 ? g->subgroup : 64;
    mm->data   = buf_create(g, ((size_t)cap + 8), 1);
    mm->start  = buf_create(g, (BUCKETS + 1) * 4, 1);
    mm->bucket = buf_create(g, (size_t)cap * 4, 1);
    mm->loc    = buf_create(g, (size_t)cap * 4 + 4, 1);
    mm->out    = buf_create(g, (size_t)cap * 4 + 4, 0);  /* cpu reads it back: cached */
    mm->pipe = pipe_create(g, "matcher.spv", 5, 16, mm->group);
    Buf bufs[5] = { mm->data, mm->start, mm->bucket, mm->loc, mm->out };
    pipe_bind(g, &mm->pipe, bufs);
    mm->ready = 1;
}

static void matcher_close(Matcher *mm) {
    if (!mm->ready) return;
    pipe_free(mm->g, &mm->pipe);
    buf_free(mm->g, &mm->out); buf_free(mm->g, &mm->loc); buf_free(mm->g, &mm->bucket);
    buf_free(mm->g, &mm->start); buf_free(mm->g, &mm->data);
    mm->ready = 0;
}

/* Fills `out` with len<<23|dist per position. */
static void matcher_run(Matcher *mm, const uint8_t *d, uint32_t n, uint32_t window,
                        uint32_t chain, uint32_t nice, uint32_t *out, int threads, double *gpu_time) {
    Match m = { .d = d, .n = n, .window = window, .chain = chain, .nice = nice, .out = out };
    build_buckets(&m);
    if (mm->ready) {
        memcpy(mm->data.p, d, n);
        memset((uint8_t *)mm->data.p + n, 0, 8);
        memcpy(mm->start.p, m.start, (BUCKETS + 1) * 4);
        if (n > 2) memcpy(mm->bucket.p, m.bucket, (size_t)(n - 2) * 4);
        memcpy(mm->loc.p, m.loc, (size_t)n * 4);
        struct { uint32_t n, window, chain, hmask, base, nice; } push =
            { n, window, chain, BUCKETS - 1, 0, nice };
        for (uint32_t base = 0; base < n; base += SLICE) {
            uint32_t m2 = n - base < SLICE ? n - base : SLICE;
            push.base = base;
            *gpu_time += pipe_run(mm->g, &mm->pipe, (m2 + mm->group - 1) / mm->group, &push, sizeof push);
        }
        memcpy(out, mm->out.p, (size_t)n * 4);
    } else {
        match_cpu(&m, threads);
    }
    free(m.start); free(m.bucket); free(m.loc);
}

/* --- pack and unpack on stdin/stdout -------------------------------------- */

static const char NVLZ_MAGIC[5] = { 'N', 'V', 'L', 'Z', '1' };

static void write_all(const void *p, size_t n) {
    if (n && fwrite(p, 1, n, stdout) != n) die("could not write to stdout");
}

static size_t read_all(uint8_t **out) {
    size_t cap = 1u << 22, n = 0;
    uint8_t *b = zalloc(cap);
    for (;;) {
        if (n == cap) { cap *= 2; b = realloc(b, cap); if (!b) die("out of memory"); }
        size_t r = fread(b + n, 1, cap - n, stdin);
        n += r;
        if (r == 0) {
            if (ferror(stdin)) die("could not read from stdin");
            break;                       /* end of file, not an error */
        }
    }
    *out = b;
    return n;
}

static int cmd_pack(Gpu *g, uint32_t window, uint32_t chain, uint32_t nice, int threads) {
    uint8_t *raw = NULL;
    size_t n = read_all(&raw);
    write_all(NVLZ_MAGIC, sizeof NVLZ_MAGIC);

    uint32_t chunk = CHUNK;
    Matcher mm;
    matcher_open(&mm, g, n < chunk ? (uint32_t)(n ? n : 1) : chunk);
    uint32_t *mt = zalloc(((size_t)(n < chunk ? n : chunk) + 1) * 4);
    double gpu_time = 0;
    size_t written = 0;

    for (size_t off = 0; off < n || off == 0; off += chunk) {
        uint32_t m = (uint32_t)(n - off < chunk ? n - off : chunk);
        matcher_run(&mm, raw + off, m, window, chain, nice, mt, threads, &gpu_time);
        Enc k;
        size_t kn = encode_chunk(raw + off, m, mt, &k);
        uint8_t header[9];
        int is_raw = kn >= m;                     /* coding gained nothing */
        header[0] = (uint8_t)(is_raw ? 1 : 0);
        uint32_t a = m, b = (uint32_t)(is_raw ? m : kn);
        memcpy(header + 1, &a, 4);
        memcpy(header + 5, &b, 4);
        write_all(header, 9);
        write_all(is_raw ? raw + off : k.out, is_raw ? m : kn);
        written += 9 + (is_raw ? m : kn);
        free(k.out);
        if (n - off <= chunk) break;
    }
    fflush(stdout);
    free(mt); free(raw);
    matcher_close(&mm);
    if (!quiet && g)
        fprintf(stderr, "nvlz: %.2f MiB -> %.2f MiB (x%.2f), gpu search %.3f s\n",
                n / 1048576.0, (written + 5) / 1048576.0,
                (double)n / (double)(written + 5), gpu_time);
    return 0;
}

static int cmd_unpack(void) {
    uint8_t *in = NULL;
    size_t n = read_all(&in);
    if (n < 5 || memcmp(in, NVLZ_MAGIC, 5)) die("not an nvlz stream");
    size_t i = 5;
    while (i < n) {
        if (i + 9 > n) die("truncated stream (chunk header)");
        int is_raw = in[i];
        uint32_t m, kn;
        memcpy(&m, in + i + 1, 4);
        memcpy(&kn, in + i + 5, 4);
        i += 9;
        if (i + kn > n) die("truncated stream (chunk)");
        if (is_raw) {
            write_all(in + i, kn);
        } else {
            uint8_t *out = zalloc(m ? m : 1);
            if (decode_chunk(in + i, kn, out, m)) die("corrupt chunk");
            write_all(out, m);
            free(out);
        }
        i += kn;
    }
    fflush(stdout);
    free(in);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* Command line                                                            */
/* ----------------------------------------------------------------------- */

static void usage(void) {
    puts("hzgpu — nvpack's gpu mode (vulkan compute)\n"
         "\n"
         "  hzgpu device                which device was picked and what it can do\n"
         "  hzgpu band [MiB]            bandwidth ceiling: gpu reads vs cpu reads\n"
         "  hzgpu hash < paths          blake2b-128 per file, bit-exact against hashlib\n"
         "  hzgpu matcher FILE          lz77 longest-match: gpu vs cpu, bit for bit\n"
         "  hzgpu selftest              known test vectors + gpu==cpu\n"
         "  hzgpu pack < raw > nv       the nvlz codec: lz77 on gpu, coding on cpu\n"
         "  hzgpu unpack < nv > raw     unpacking (pure cpu, needs no gpu)\n"
         "\n"
         "flags:\n"
         "  --cpu           run the cpu path only (for the comparison)\n"
         "  --jobs N        cpu threads (default: all)\n"
         "  --window N      the matcher's window in bytes (default 32768)\n"
         "  --chain N       how many candidates the matcher tries (default 4096)\n"
         "  --nice N        a match this long is enough, stop looking (default 258)\n"
         "  --bigfile MiB   files bigger than this are hashed on cpu (default 16)\n"
         "  --batch MiB     gpu buffer size per batch (default 192)\n"
         "  --cap MiB       read at most this much of the file in `matcher` (default 64)\n"
         "  --quiet         errors on output only\n"
         "\n"
         "The Swedish command and flag names (packa, packaupp, hasha, enhet, prov,\n"
         "--tyst, --jobb, --fonster, --kedja, --nog, --lzfonster, --storfil,\n"
         "--parti, --tak) are accepted as aliases, because scripts call them.\n"
         "\n"
         "NVGPU_DEVICE=n picks vulkan device n instead of the automatic choice\n"
         "(the old name NVGPU_ENHET still works).");
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--help") || !strcmp(argv[1], "--hjälp") || !strcmp(argv[1], "-h")) {
        usage(); return argc < 2 ? 2 : 0;
    }

    const char *cmd = argv[1];
    /* Swedish aliases for the subcommands: they were the original names and
     * existing scripts still call them.  Alias strings are data; the
     * canonical names inside this file are the English ones. */
    if (!strcmp(cmd, "packa")) cmd = "pack";
    else if (!strcmp(cmd, "packaupp")) cmd = "unpack";
    else if (!strcmp(cmd, "hasha")) cmd = "hash";
    else if (!strcmp(cmd, "enhet")) cmd = "device";
    else if (!strcmp(cmd, "prov")) cmd = "selftest";
    int cpu_only = 0;
    int threads = (int)sysconf(_SC_NPROCESSORS_ONLN);
    uint32_t window = 32768, chain = 4096, nice = 258, lz_window = 0;
    /* `matcher` measures and must therefore run every chain to the bottom.
     * `pack` is supposed to pack, and there the defaults are the measured
     * ones: chain 64 and nice 96 on a four-megabyte window gave x8.0 on the
     * code corpus, while chain 4096 gave x8.3 for three times the time.
     * The flags override them. */
    int chain_set = 0, nice_set = 0;
    double bigfile = 16, batch = 192, cap = 64, band_mib = 512;
    const char *file = NULL;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        int has_value = i + 1 < argc;
        if (!strcmp(a, "--cpu")) cpu_only = 1;
        else if (!strcmp(a, "--quiet") || !strcmp(a, "--tyst")) quiet = 1;
        else if ((!strcmp(a, "--jobs") || !strcmp(a, "--jobb")) && has_value) threads = atoi(argv[++i]);
        else if ((!strcmp(a, "--window") || !strcmp(a, "--fonster")) && has_value) window = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if ((!strcmp(a, "--chain") || !strcmp(a, "--kedja")) && has_value) { chain = (uint32_t)strtoul(argv[++i], NULL, 10); chain_set = 1; }
        else if ((!strcmp(a, "--nice") || !strcmp(a, "--nog")) && has_value) { nice = (uint32_t)strtoul(argv[++i], NULL, 10); nice_set = 1; }
        else if ((!strcmp(a, "--lzwindow") || !strcmp(a, "--lzfonster")) && has_value) lz_window = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if ((!strcmp(a, "--bigfile") || !strcmp(a, "--storfil")) && has_value) bigfile = atof(argv[++i]);
        else if ((!strcmp(a, "--batch") || !strcmp(a, "--parti")) && has_value) batch = atof(argv[++i]);
        else if ((!strcmp(a, "--cap") || !strcmp(a, "--tak")) && has_value) cap = atof(argv[++i]);
        else if (a[0] == '-') die("unknown flag %s (see --help)", a);
        else if (!file) file = a;
        else die("too many arguments: %s", a);
    }
    if (threads < 1) threads = 1;
    if (window > (1u << 23) - 1) die("--window may be at most %u", (1u << 23) - 1);

    if (!strcmp(cmd, "band") && file) band_mib = atof(file);

    /* --cpu must work even on a machine with no gpu at all. */
    Gpu g;
    int need_gpu = strcmp(cmd, "unpack") != 0
        && !(cpu_only && (!strcmp(cmd, "hash") || !strcmp(cmd, "matcher") || !strcmp(cmd, "pack")));
    if (need_gpu) gpu_open(&g);

    int ret;
    if (!strcmp(cmd, "device")) ret = cmd_device(&g);
    else if (!strcmp(cmd, "band")) ret = cmd_band(&g, band_mib, threads);
    else if (!strcmp(cmd, "hash")) ret = cmd_hash(need_gpu ? &g : NULL, cpu_only, threads,
                                                  (size_t)(batch * 1048576.0), (size_t)(bigfile * 1048576.0));
    else if (!strcmp(cmd, "matcher")) {
        if (!file) die("matcher needs a file");
        ret = cmd_matcher(need_gpu ? &g : NULL, file, window, chain, nice, threads, cpu_only, cap);
    }
    else if (!strcmp(cmd, "selftest")) ret = cmd_selftest(&g, threads);
    else if (!strcmp(cmd, "pack"))
        ret = cmd_pack(need_gpu ? &g : NULL, lz_window ? lz_window : LZ_WINDOW,
                       chain_set ? chain : 64, nice_set ? nice : 96, threads);
    else if (!strcmp(cmd, "unpack")) ret = cmd_unpack();
    else { usage(); return 2; }

    if (need_gpu) gpu_close(&g);
    return ret;
}
