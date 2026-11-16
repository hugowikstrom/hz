/* nvgpu — GPU-läget i nvpack: Vulkan-compute mot den integrerade grafiken.
 *
 * VARFÖR DEN FINNS. Frågan var om ett GPU-läge gör packningen snabbare. Den
 * frågan går inte att svara på med en åsikt: nvpack gör fyra sorters arbete,
 * och bara två av dem har GPU-form. Det här programmet kör dem BÅDA på GPU:n
 * och på CPU:n med samma semantik, jämför svaren bit för bit och mäter tiden,
 * så att svaret blir en mätning i stället för ett antagande.
 *
 *   nvgpu enhet            vilken enhet som valdes och vad den klarar
 *   nvgpu band [MiB]       bandbreddstak: GPU-läsning mot CPU-läsning
 *   nvgpu hasha            blake2b-128 per fil (sökvägar på stdin), bitexakt
 *   nvgpu matcher FIL      LZ77-längsta-matchning: GPU mot CPU, bit för bit
 *   nvgpu prov             självprov: kända testvektorer + GPU==CPU
 *
 * Flaggor: --cpu (bara CPU-vägen), --tyst, --jobb N (CPU-trådar),
 *          --fonster N, --kedja N (matcherns fönster och kedjetak).
 *
 * BITEXAKTHET. Hashen måste vara exakt samma funktion som pythondelen
 * använder (hashlib.blake2b, digest_size=16), annars pekar dedupen fel och
 * uppackningens kontroll av objekthashar faller. Därför finns blake2b även i
 * C här, och `prov` kör GPU mot CPU mot de officiella testvektorerna.
 *
 * Matcharen är nvpacks EGEN — den behöver inte likna zlibs, för ingen
 * uppackare utanför nvpack ska läsa den. Den letar i hinkar (en hink per
 * 3-bytehash) som byggs på CPU:n i stigande ordning, och väljer längsta
 * matchningen, vid lika längd den närmaste. GPU och CPU går igenom samma
 * kandidater i samma ordning, så utdata MÅSTE bli identisk — och det är
 * precis vad `matcher` kontrollerar innan den redovisar någon tid.
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
/* Smått                                                                   */
/* ----------------------------------------------------------------------- */

static int TYST = 0;

static void dö(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    fputs("nvgpu: ", stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr);
    va_end(ap); exit(2);
}

static void säg(const char *fmt, ...) {
    if (TYST) return;
    va_list ap; va_start(ap, fmt);
    vfprintf(stdout, fmt, ap); va_end(ap); fflush(stdout);
}

static double nu(void) {
    struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}

#define VK(uttryck) do { VkResult _r = (uttryck); \
    if (_r != VK_SUCCESS) dö("%s rad %d: vulkan-fel %d", __func__, __LINE__, (int)_r); } while (0)

static void *kalla(size_t n) {
    void *p = calloc(1, n ? n : 1);
    if (!p) dö("minnet tog slut (%zu byte)", n);
    return p;
}

/* ----------------------------------------------------------------------- */
/* blake2b-128 på CPU — referensen som GPU:n måste träffa                  */
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

static void b2b_komprimera(uint64_t h[8], const uint8_t blk[128], uint64_t t, int sista) {
    uint64_t v[16], m[16];
    for (int i = 0; i < 16; i++) {
        uint64_t w = 0;
        for (int j = 7; j >= 0; j--) w = (w << 8) | blk[i * 8 + j];
        m[i] = w;
    }
    for (int i = 0; i < 8; i++) v[i] = h[i];
    for (int i = 0; i < 8; i++) v[8 + i] = B2B_IV[i];
    v[12] ^= t;
    if (sista) v[14] = ~v[14];
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

/* blake2b med utlängd 16, utan nyckel — samma som hashlib.blake2b(digest_size=16) */
static void blake2b128(const uint8_t *data, size_t n, uint8_t ut[16]) {
    uint64_t h[8];
    for (int i = 0; i < 8; i++) h[i] = B2B_IV[i];
    h[0] ^= 0x01010000ULL ^ 16ULL;          /* fanout=1, djup=1, nyckellängd=0, utlängd=16 */
    size_t i = 0;
    uint64_t t = 0;
    while (n - i > 128) {                    /* alla hela block UTOM det sista */
        t += 128;
        b2b_komprimera(h, data + i, t, 0);
        i += 128;
    }
    uint8_t sista[128];
    memset(sista, 0, 128);
    size_t kvar = n - i;
    memcpy(sista, data + i, kvar);
    t += kvar;
    b2b_komprimera(h, sista, t, 1);
    for (int k = 0; k < 16; k++) ut[k] = (uint8_t)(h[k / 8] >> (8 * (k % 8)));
}

/* ----------------------------------------------------------------------- */
/* Vulkan-stommen                                                          */
/* ----------------------------------------------------------------------- */

typedef struct {
    VkInstance inst;
    VkPhysicalDevice fys;
    VkDevice dev;
    VkQueue kö;
    uint32_t köfamilj;
    VkCommandPool pool;
    VkPhysicalDeviceProperties egenskaper;
    VkPhysicalDeviceMemoryProperties minne;
    uint32_t subgrupp;
    char spvkatalog[PATH_MAX];
} Gpu;

typedef struct {
    VkBuffer buf;
    VkDeviceMemory mem;
    void *p;                 /* mappad hela tiden — på en APU är det samma RAM */
    VkDeviceSize storlek;
    int enhetsnära;          /* låg i DEVICE_LOCAL också */
} Buf;

typedef struct {
    VkDescriptorSetLayout utlägg;
    VkPipelineLayout rörlägg;
    VkPipeline rör;
    VkDescriptorPool dpool;
    VkDescriptorSet dset;
    uint32_t antal_bindningar;
} Rör;

/* Katalogen där .spv-filerna ligger: samma som den egna binären. */
static void hitta_spvkatalog(Gpu *g) {
    char sökväg[PATH_MAX];
    ssize_t n = readlink("/proc/self/exe", sökväg, sizeof sökväg - 1);
    if (n <= 0) dö("kan inte hitta min egen sökväg");
    sökväg[n] = 0;
    char *snedstreck = strrchr(sökväg, '/');
    if (!snedstreck) dö("egen sökväg utan katalog: %s", sökväg);
    *snedstreck = 0;
    snprintf(g->spvkatalog, sizeof g->spvkatalog, "%s", sökväg);
}

static void gpu_öppna(Gpu *g) {
    memset(g, 0, sizeof *g);
    hitta_spvkatalog(g);

    VkApplicationInfo ai = { .sType = VK_STRUCTURE_TYPE_APPLICATION_INFO,
                             .pApplicationName = "nvgpu",
                             .apiVersion = VK_API_VERSION_1_3 };
    VkInstanceCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO,
                                .pApplicationInfo = &ai };
    VK(vkCreateInstance(&ci, NULL, &g->inst));

    uint32_t antal = 0;
    VK(vkEnumeratePhysicalDevices(g->inst, &antal, NULL));
    if (!antal) dö("ingen vulkan-enhet alls");
    VkPhysicalDevice *fys = kalla(antal * sizeof *fys);
    VK(vkEnumeratePhysicalDevices(g->inst, &antal, fys));

    /* Val: NVGPU_ENHET om satt, annars första riktiga grafiken. llvmpipe
     * (PHYSICAL_DEVICE_TYPE_CPU) duger inte — den ÄR cpu:n, och då mäter vi
     * cpu mot cpu och tror att vi mätt en gpu. */
    const char *val = getenv("NVGPU_ENHET");
    int vald = -1;
    for (uint32_t i = 0; i < antal; i++) {
        VkPhysicalDeviceProperties p;
        vkGetPhysicalDeviceProperties(fys[i], &p);
        if (val) { if ((uint32_t)atoi(val) == i) { vald = (int)i; break; } continue; }
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) { vald = (int)i; break; }
        if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU && vald < 0) vald = (int)i;
    }
    if (vald < 0) dö("hittade ingen gpu (bara programvarurendering); sätt NVGPU_ENHET för att tvinga");
    g->fys = fys[vald];
    free(fys);

    vkGetPhysicalDeviceProperties(g->fys, &g->egenskaper);
    vkGetPhysicalDeviceMemoryProperties(g->fys, &g->minne);

    VkPhysicalDeviceSubgroupProperties sg = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SUBGROUP_PROPERTIES };
    VkPhysicalDeviceProperties2 p2 = { .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &sg };
    vkGetPhysicalDeviceProperties2(g->fys, &p2);
    g->subgrupp = sg.subgroupSize;

    VkPhysicalDeviceFeatures funktioner;
    vkGetPhysicalDeviceFeatures(g->fys, &funktioner);
    if (!funktioner.shaderInt64) dö("enheten saknar shaderInt64 — blake2b går inte att köra här");

    uint32_t köer = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(g->fys, &köer, NULL);
    VkQueueFamilyProperties *kf = kalla(köer * sizeof *kf);
    vkGetPhysicalDeviceQueueFamilyProperties(g->fys, &köer, kf);
    int familj = -1;
    for (uint32_t i = 0; i < köer; i++)
        if (kf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) { familj = (int)i; break; }
    free(kf);
    if (familj < 0) dö("enheten har ingen beräkningskö");
    g->köfamilj = (uint32_t)familj;

    float prio = 1.0f;
    VkDeviceQueueCreateInfo qi = { .sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO,
                                   .queueFamilyIndex = g->köfamilj,
                                   .queueCount = 1, .pQueuePriorities = &prio };
    VkPhysicalDeviceFeatures vill = { .shaderInt64 = VK_TRUE };
    VkDeviceCreateInfo di = { .sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO,
                              .queueCreateInfoCount = 1, .pQueueCreateInfos = &qi,
                              .pEnabledFeatures = &vill };
    VK(vkCreateDevice(g->fys, &di, NULL, &g->dev));
    vkGetDeviceQueue(g->dev, g->köfamilj, 0, &g->kö);

    VkCommandPoolCreateInfo pi = { .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                                   .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                                   .queueFamilyIndex = g->köfamilj };
    VK(vkCreateCommandPool(g->dev, &pi, NULL, &g->pool));
}

static void gpu_stäng(Gpu *g) {
    vkDestroyCommandPool(g->dev, g->pool, NULL);
    vkDestroyDevice(g->dev, NULL);
    vkDestroyInstance(g->inst, NULL);
}

/* Minnestyp. Alltid host-visible (vi mappar allt), men sedan skiljer det sig
 * åt beroende på VEM som läser bufferten mest:
 *
 *   enhetsnära=1  DEVICE_LOCAL — det gpu:n betar av. På en apu är det samma
 *                 fysiska RAM som cpu:ns, så ingen kopia behövs, men minnet
 *                 är write-combined och därmed OCACHAT från cpu-sidan.
 *   enhetsnära=0  HOST_CACHED — det cpu:n läser TILLBAKA. Mätningen av
 *                 bandbredden säger att cpu:n läser det delade (wc) minnet
 *                 med 441 MiB/s mot 30 788 MiB/s för vanligt minne: 70 gånger
 *                 långsammare. Att läsa hem 64 MiB matchningar ur wc-minne
 *                 kostade mer än gpu:n hade sparat på att hitta dem, och
 *                 gjorde gpu-vägen långsammare än sitt eget cpu-fallback.
 */
static uint32_t minnestyp(Gpu *g, uint32_t tillåtna, int vill_enhetsnära, int *fick_enhetsnära) {
    VkMemoryPropertyFlags måste = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkMemoryPropertyFlags helst = vill_enhetsnära ? VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT
                                                  : VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    for (int runda = 0; runda < 2; runda++) {
        VkMemoryPropertyFlags önskat = måste | (runda == 0 ? helst : 0);
        for (uint32_t i = 0; i < g->minne.memoryTypeCount; i++) {
            if (!(tillåtna & (1u << i))) continue;
            if ((g->minne.memoryTypes[i].propertyFlags & önskat) == önskat) {
                if (fick_enhetsnära) *fick_enhetsnära = (runda == 0 && vill_enhetsnära);
                return i;
            }
        }
    }
    dö("ingen användbar minnestyp (host-visible krävs)");
    return 0;
}

static Buf buf_skapa(Gpu *g, VkDeviceSize storlek, int enhetsnära) {
    Buf b = { .storlek = storlek ? storlek : 4 };
    VkBufferCreateInfo bi = { .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
                              .size = b.storlek,
                              .usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                              .sharingMode = VK_SHARING_MODE_EXCLUSIVE };
    VK(vkCreateBuffer(g->dev, &bi, NULL, &b.buf));
    VkMemoryRequirements krav;
    vkGetBufferMemoryRequirements(g->dev, b.buf, &krav);
    VkMemoryAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
                                .allocationSize = krav.size,
                                .memoryTypeIndex = minnestyp(g, krav.memoryTypeBits, enhetsnära, &b.enhetsnära) };
    VK(vkAllocateMemory(g->dev, &ai, NULL, &b.mem));
    VK(vkBindBufferMemory(g->dev, b.buf, b.mem, 0));
    VK(vkMapMemory(g->dev, b.mem, 0, VK_WHOLE_SIZE, 0, &b.p));
    return b;
}

static void buf_fria(Gpu *g, Buf *b) {
    if (!b->buf) return;
    vkUnmapMemory(g->dev, b->mem);
    vkDestroyBuffer(g->dev, b->buf, NULL);
    vkFreeMemory(g->dev, b->mem, NULL);
    memset(b, 0, sizeof *b);
}

static uint32_t *läs_spv(Gpu *g, const char *namn, size_t *byte) {
    char sökväg[PATH_MAX + 64];
    snprintf(sökväg, sizeof sökväg, "%s/%s", g->spvkatalog, namn);
    FILE *f = fopen(sökväg, "rb");
    if (!f) dö("hittar inte %s — kör `make` först", sökväg);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0 || n % 4) dö("%s är ingen spir-v-modul (%ld byte)", sökväg, n);
    uint32_t *kod = kalla((size_t)n);
    if (fread(kod, 1, (size_t)n, f) != (size_t)n) dö("kunde inte läsa %s", sökväg);
    fclose(f);
    *byte = (size_t)n;
    return kod;
}

static Rör rör_skapa(Gpu *g, const char *spv, uint32_t antal_bindningar, uint32_t push_byte,
                     uint32_t gruppstorlek) {
    Rör r = { .antal_bindningar = antal_bindningar };
    VkDescriptorSetLayoutBinding *b = kalla(antal_bindningar * sizeof *b);
    for (uint32_t i = 0; i < antal_bindningar; i++)
        b[i] = (VkDescriptorSetLayoutBinding){ .binding = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,
            .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT };
    VkDescriptorSetLayoutCreateInfo li = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                                           .bindingCount = antal_bindningar, .pBindings = b };
    VK(vkCreateDescriptorSetLayout(g->dev, &li, NULL, &r.utlägg));
    free(b);

    VkPushConstantRange push = { .stageFlags = VK_SHADER_STAGE_COMPUTE_BIT, .offset = 0, .size = push_byte };
    VkPipelineLayoutCreateInfo pli = { .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
                                       .setLayoutCount = 1, .pSetLayouts = &r.utlägg,
                                       .pushConstantRangeCount = push_byte ? 1 : 0,
                                       .pPushConstantRanges = push_byte ? &push : NULL };
    VK(vkCreatePipelineLayout(g->dev, &pli, NULL, &r.rörlägg));

    size_t spvbyte = 0;
    uint32_t *kod = läs_spv(g, spv, &spvbyte);
    VkShaderModuleCreateInfo smi = { .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
                                     .codeSize = spvbyte, .pCode = kod };
    VkShaderModule modul;
    VK(vkCreateShaderModule(g->dev, &smi, NULL, &modul));
    free(kod);

    /* Gruppstorleken är en specialiseringskonstant (id 0) så samma shader kan
     * köras med den bredd enheten faktiskt vill ha. */
    VkSpecializationMapEntry post = { .constantID = 0, .offset = 0, .size = sizeof(uint32_t) };
    VkSpecializationInfo spec = { .mapEntryCount = 1, .pMapEntries = &post,
                                  .dataSize = sizeof(uint32_t), .pData = &gruppstorlek };
    VkComputePipelineCreateInfo ci = { .sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO,
        .stage = { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
                   .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = modul, .pName = "main",
                   .pSpecializationInfo = &spec },
        .layout = r.rörlägg };
    VK(vkCreateComputePipelines(g->dev, VK_NULL_HANDLE, 1, &ci, NULL, &r.rör));
    vkDestroyShaderModule(g->dev, modul, NULL);

    VkDescriptorPoolSize storlek = { .type = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .descriptorCount = antal_bindningar };
    VkDescriptorPoolCreateInfo dpi = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                                       .maxSets = 1, .poolSizeCount = 1, .pPoolSizes = &storlek };
    VK(vkCreateDescriptorPool(g->dev, &dpi, NULL, &r.dpool));
    VkDescriptorSetAllocateInfo dai = { .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                                        .descriptorPool = r.dpool, .descriptorSetCount = 1,
                                        .pSetLayouts = &r.utlägg };
    VK(vkAllocateDescriptorSets(g->dev, &dai, &r.dset));
    return r;
}

static void rör_fria(Gpu *g, Rör *r) {
    vkDestroyDescriptorPool(g->dev, r->dpool, NULL);
    vkDestroyPipeline(g->dev, r->rör, NULL);
    vkDestroyPipelineLayout(g->dev, r->rörlägg, NULL);
    vkDestroyDescriptorSetLayout(g->dev, r->utlägg, NULL);
}

static void rör_bind(Gpu *g, Rör *r, Buf *bufar) {
    VkDescriptorBufferInfo *info = kalla(r->antal_bindningar * sizeof *info);
    VkWriteDescriptorSet *skriv = kalla(r->antal_bindningar * sizeof *skriv);
    for (uint32_t i = 0; i < r->antal_bindningar; i++) {
        info[i] = (VkDescriptorBufferInfo){ .buffer = bufar[i].buf, .offset = 0, .range = VK_WHOLE_SIZE };
        skriv[i] = (VkWriteDescriptorSet){ .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = r->dset, .dstBinding = i, .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &info[i] };
    }
    vkUpdateDescriptorSets(g->dev, r->antal_bindningar, skriv, 0, NULL);
    free(info); free(skriv);
}

/* Så många positioner en matcharkärna får i ETT anrop. Drivrutinens vakthund
 * dödar en kärna som maler för länge, och gpu:n sitter i samma krets som
 * skärmen: en hård återställning tar skrivbordet med sig. 256 Ki positioner
 * är några tiondelar av en sekund även på data där varje kedja går till
 * taket. */
#define SKIVA (256u << 10)

/* Kör en shader och vänta ut den. Returnerar väggtiden för submit+vänta,
 * alltså vad en anropare faktiskt betalar — inte en städad kärntid. */
static double rör_kör(Gpu *g, Rör *r, uint32_t grupper, const void *push, uint32_t push_byte) {
    VkCommandBufferAllocateInfo ai = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .commandPool = g->pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1 };
    VkCommandBuffer cb;
    VK(vkAllocateCommandBuffers(g->dev, &ai, &cb));
    VkCommandBufferBeginInfo bi = { .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
                                    .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT };
    VK(vkBeginCommandBuffer(cb, &bi));
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->rör);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, r->rörlägg, 0, 1, &r->dset, 0, NULL);
    if (push_byte) vkCmdPushConstants(cb, r->rörlägg, VK_SHADER_STAGE_COMPUTE_BIT, 0, push_byte, push);
    vkCmdDispatch(cb, grupper, 1, 1);
    VK(vkEndCommandBuffer(cb));

    VkFenceCreateInfo fi = { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
    VkFence klar;
    VK(vkCreateFence(g->dev, &fi, NULL, &klar));
    VkSubmitInfo si = { .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb };
    double t0 = nu();
    VK(vkQueueSubmit(g->kö, 1, &si, klar));
    VK(vkWaitForFences(g->dev, 1, &klar, VK_TRUE, 120ULL * 1000 * 1000 * 1000));
    double dt = nu() - t0;
    vkDestroyFence(g->dev, klar, NULL);
    vkFreeCommandBuffers(g->dev, g->pool, 1, &cb);
    return dt;
}

/* ----------------------------------------------------------------------- */
/* Filer och trådar                                                        */
/* ----------------------------------------------------------------------- */

typedef struct { uint8_t *p; size_t n; int fd; } Fil;

static Fil fil_öppna(const char *sökväg) {
    Fil f = { .fd = -1 };
    int fd = open(sökväg, O_RDONLY);
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

static void fil_stäng(Fil *f) {
    if (f->n && f->p) munmap(f->p, f->n);
    if (f->fd >= 0) close(f->fd);
    f->p = NULL; f->fd = -1; f->n = 0;
}

typedef struct {
    void (*gör)(void *ctx, size_t i);
    void *ctx;
    size_t n;
    size_t nästa;
    pthread_mutex_t lås;
} Svärm;

static void *svärm_tråd(void *arg) {
    Svärm *s = arg;
    for (;;) {
        pthread_mutex_lock(&s->lås);
        size_t i = s->nästa < s->n ? s->nästa++ : (size_t)-1;
        pthread_mutex_unlock(&s->lås);
        if (i == (size_t)-1) return NULL;
        s->gör(s->ctx, i);
    }
}

/* Kör gör(ctx, 0..n-1) över `trådar` trådar. Uppgifterna delas ut en i taget,
 * för filstorlekar är allt annat än jämnt fördelade. */
static void svärm_kör(void (*gör)(void *, size_t), void *ctx, size_t n, int trådar) {
    if (n == 0) return;
    if (trådar < 1) trådar = 1;
    if ((size_t)trådar > n) trådar = (int)n;
    Svärm s = { .gör = gör, .ctx = ctx, .n = n, .nästa = 0 };
    pthread_mutex_init(&s.lås, NULL);
    pthread_t *t = kalla((size_t)trådar * sizeof *t);
    for (int i = 0; i < trådar; i++) pthread_create(&t[i], NULL, svärm_tråd, &s);
    for (int i = 0; i < trådar; i++) pthread_join(t[i], NULL);
    free(t);
    pthread_mutex_destroy(&s.lås);
}

/* ----------------------------------------------------------------------- */
/* LZ77-matchning: hinkar, CPU-sökning                                     */
/* ----------------------------------------------------------------------- */

#define HBITAR 15u
#define HINKAR (1u << HBITAR)

typedef struct {
    const uint8_t *d;
    uint32_t n;
    uint32_t *start;      /* HINKAR + 1 */
    uint32_t *hink;       /* positioner, stigande inom varje hink */
    uint32_t *plats;      /* absolut index i `hink` för varje position */
    uint32_t fönster, kedja, nog;
    uint32_t *ut;         /* len<<23 | dist */
} Matchning;

static inline uint32_t hash3(const uint8_t *d, uint32_t i) {
    uint32_t h = ((uint32_t)d[i] << 10) ^ ((uint32_t)d[i + 1] << 5) ^ (uint32_t)d[i + 2];
    return (uint32_t)((h * 2654435761u) >> 15) & (HINKAR - 1u);
}

/* Hinkarna byggs på CPU: det är ett enda svep, och GPU:n ska mäta
 * SÖKNINGEN, inte en uppställning som ändå kostar samma på båda sidor. */
static void bygg_hinkar(Matchning *m) {
    uint32_t n = m->n;
    uint32_t pos_antal = n > 2 ? n - 2 : 0;
    m->start = kalla((HINKAR + 1) * sizeof *m->start);
    m->hink = kalla((size_t)(pos_antal ? pos_antal : 1) * sizeof *m->hink);
    m->plats = kalla((size_t)(n ? n : 1) * sizeof *m->plats);
    for (uint32_t i = 0; i < pos_antal; i++) m->start[hash3(m->d, i) + 1]++;
    for (uint32_t h = 0; h < HINKAR; h++) m->start[h + 1] += m->start[h];
    uint32_t *markör = kalla(HINKAR * sizeof *markör);
    for (uint32_t h = 0; h < HINKAR; h++) markör[h] = m->start[h];
    for (uint32_t i = 0; i < pos_antal; i++) {
        uint32_t h = hash3(m->d, i);
        uint32_t idx = markör[h]++;
        m->hink[idx] = i;
        m->plats[i] = idx;
    }
    free(markör);
}

static void matcha_del(void *ctx, size_t bit) {
    Matchning *m = ctx;
    uint32_t per = 1u << 16;
    uint32_t från = (uint32_t)bit * per;
    uint32_t till = från + per < m->n ? från + per : m->n;
    for (uint32_t pos = från; pos < till; pos++) {
        if (pos + 2 >= m->n) { m->ut[pos] = 0; continue; }
        uint32_t tak = m->n - pos < 258 ? m->n - pos : 258;
        uint32_t h = hash3(m->d, pos);
        uint32_t i = m->plats[pos], bas = m->start[h];
        uint32_t undre = pos > m->fönster ? pos - m->fönster : 0;
        uint32_t bäst_len = 0, bäst_dist = 0, provade = 0;
        while (i > bas && provade < m->kedja) {
            i--;
            uint32_t c = m->hink[i];
            if (c < undre) break;
            provade++;
            if (bäst_len >= 1 && m->d[c + bäst_len] != m->d[pos + bäst_len]) continue;
            uint32_t l = 0;
            while (l < tak && m->d[c + l] == m->d[pos + l]) l++;
            if (l > bäst_len) {
                bäst_len = l; bäst_dist = pos - c;
                if (l >= tak || l >= m->nog) break;      /* nog är nog */
            }
        }
        m->ut[pos] = bäst_len >= 3 ? (bäst_len << 23) | bäst_dist : 0;
    }
}

static double matcha_cpu(Matchning *m, int trådar) {
    size_t bitar = ((size_t)m->n + (1u << 16) - 1) / (1u << 16);
    double t0 = nu();
    svärm_kör(matcha_del, m, bitar ? bitar : 1, trådar);
    return nu() - t0;
}

/* ----------------------------------------------------------------------- */
/* Kommandon                                                               */
/* ----------------------------------------------------------------------- */

static const char *minnesnamn(VkMemoryPropertyFlags f) {
    static char b[128];
    snprintf(b, sizeof b, "%s%s%s%s",
             (f & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT) ? "enhet " : "",
             (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) ? "synligt " : "",
             (f & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT) ? "sammanhängande " : "",
             (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT) ? "cachat" : "");
    return b;
}

static int kmd_enhet(Gpu *g) {
    säg("enhet: %s\n", g->egenskaper.deviceName);
    const char *typ = g->egenskaper.deviceType == VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU ? "integrerad"
                    : g->egenskaper.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU ? "diskret"
                    : "annan";
    säg("  typ               %s\n", typ);
    säg("  drivrutin         vulkan %u.%u.%u\n",
        VK_VERSION_MAJOR(g->egenskaper.apiVersion), VK_VERSION_MINOR(g->egenskaper.apiVersion),
        VK_VERSION_PATCH(g->egenskaper.apiVersion));
    säg("  subgrupp          %u trådar\n", g->subgrupp);
    säg("  arbetsgrupp max   %u trådar, %u byte delat minne\n",
        g->egenskaper.limits.maxComputeWorkGroupInvocations,
        g->egenskaper.limits.maxComputeSharedMemorySize);
    säg("  buffert max       %.1f GiB\n", g->egenskaper.limits.maxStorageBufferRange / 1073741824.0);
    for (uint32_t i = 0; i < g->minne.memoryHeapCount; i++)
        säg("  hög %u            %.1f GiB%s\n", i, g->minne.memoryHeaps[i].size / 1073741824.0,
            (g->minne.memoryHeaps[i].flags & VK_MEMORY_HEAP_DEVICE_LOCAL_BIT) ? " (enhetsnära)" : "");
    Buf b = buf_skapa(g, 1 << 20, 1);
    säg("  nollkopiering     %s\n", b.enhetsnära
        ? "ja — enhetsnära minne är mappbart (delat RAM)" : "nej — måste kopiera");
    for (uint32_t i = 0; i < g->minne.memoryTypeCount; i++) {
        VkMemoryPropertyFlags f = g->minne.memoryTypes[i].propertyFlags;
        if ((f & (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
            == (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            säg("  minnestyp %u       %s\n", i, minnesnamn(f));
            break;
        }
    }
    buf_fria(g, &b);
    return 0;
}

typedef struct { const uint32_t *d; uint32_t ord; uint64_t *summor; } Läs;

static void läs_del(void *ctx, size_t i) {
    Läs *l = ctx;
    uint32_t per = 1u << 20;
    uint32_t från = (uint32_t)i * per;
    uint32_t till = från + per < l->ord ? från + per : l->ord;
    uint64_t s = 0;
    for (uint32_t k = från; k < till; k++) s += l->d[k];
    l->summor[i] = s;
}

static int kmd_band(Gpu *g, double mib, int trådar) {
    uint32_t ord = (uint32_t)(mib * 1048576.0 / 4.0);
    Buf data = buf_skapa(g, (VkDeviceSize)ord * 4, 1);
    uint32_t *d = data.p;
    for (uint32_t i = 0; i < ord; i++) d[i] = i * 2654435761u;

    uint32_t grupper = 512, grupp = 64;
    Buf ut = buf_skapa(g, (VkDeviceSize)grupper * grupp * 4, 1);
    Rör r = rör_skapa(g, "band.spv", 2, 8, grupp);
    Buf bufar[2] = { data, ut };
    rör_bind(g, &r, bufar);
    struct { uint32_t ord, varv; } push = { ord, 1 };

    rör_kör(g, &r, grupper, &push, sizeof push);          /* värm upp */
    double bäst = 1e30;
    for (int i = 0; i < 3; i++) {
        double dt = rör_kör(g, &r, grupper, &push, sizeof push);
        if (dt < bäst) bäst = dt;
    }
    double gpu = mib / bäst;

    /* CPU-sidan läser VANLIGT minne. Att låta den läsa gpu-bufferten vore att
     * mäta fel sak: det delade minnet är write-combined, alltså ocachat från
     * cpu-sidan, och en cpu som läser ocachat minne går på knäna. Det är
     * priset för "nollkopiering" på en apu, och det mäts separat nedan. */
    size_t bitar = ((size_t)ord + (1u << 20) - 1) / (1u << 20);
    uint32_t *vanligt = kalla((size_t)ord * 4);
    memcpy(vanligt, d, (size_t)ord * 4);
    Läs l = { .d = vanligt, .ord = ord, .summor = kalla(bitar * sizeof(uint64_t)) };
    double cbäst = 1e30;
    for (int i = 0; i < 3; i++) {
        double t0 = nu();
        svärm_kör(läs_del, &l, bitar, trådar);
        double dt = nu() - t0;
        if (dt < cbäst) cbäst = dt;
    }
    double cpu = mib / cbäst;

    Läs l2 = { .d = d, .ord = ord, .summor = l.summor };
    double wbäst = 1e30;
    for (int i = 0; i < 2; i++) {
        double t0 = nu();
        svärm_kör(läs_del, &l2, bitar, trådar);
        double dt = nu() - t0;
        if (dt < wbäst) wbäst = dt;
    }
    double wc = mib / wbäst;
    free(l.summor); free(vanligt);

    säg("band: %.0f MiB läst\n", mib);
    säg("  gpu läser delat minne       %7.3f s  %8.0f MiB/s\n", bäst, gpu);
    säg("  cpu läser vanligt minne     %7.3f s  %8.0f MiB/s  (%d trådar)\n", cbäst, cpu, trådar);
    säg("  cpu läser delat minne       %7.3f s  %8.0f MiB/s  (ocachat/wc — %.0fx straff)\n",
        wbäst, wc, cpu / wc);
    säg("  gpu/cpu %.2fx\n", gpu / cpu);
    rör_fria(g, &r);
    buf_fria(g, &ut);
    buf_fria(g, &data);
    return 0;
}

/* --- hasha -------------------------------------------------------------- */

typedef struct {
    char **sökvägar;
    size_t antal;
    uint8_t *hashar;      /* 16 byte per fil */
    uint64_t *längder;
    int *hoppat;          /* 1 = för stor för gpu, togs på cpu */
} Hashjobb;

static void hasha_cpu_en(void *ctx, size_t i) {
    Hashjobb *h = ctx;
    Fil f = fil_öppna(h->sökvägar[i]);
    if (f.fd < 0 && f.n == 0 && !f.p) { memset(&h->hashar[i * 16], 0, 16); h->längder[i] = 0; return; }
    blake2b128(f.p, f.n, &h->hashar[i * 16]);
    h->längder[i] = f.n;
    fil_stäng(&f);
}

static void skriv_hashar(Hashjobb *h) {
    for (size_t i = 0; i < h->antal; i++) {
        char hex[33];
        for (int k = 0; k < 16; k++) snprintf(hex + k * 2, 3, "%02x", h->hashar[i * 16 + k]);
        printf("%s  %s\n", hex, h->sökvägar[i]);
    }
}

/* GPU-vägen: filerna kopieras in i en delad buffert i partier, en tråd per
 * fil i kärnan. Kopieringen RÄKNAS in i tiden — den är vad gpu-vägen faktiskt
 * kostar en anropare, och att inte räkna den vore att mäta fel sak.
 *
 * Insamlingen är TRÅDAD. Första versionen läste filerna i en tråd och
 * jämförde med en cpu-väg som hade 24: det gav gpu:n 6 MiB/s mot cpu:ns 693
 * och var ett mått på min egen kod, inte på hårdvaran. Kärnan tog 0,038 s av
 * de 8,8 sekunderna.
 */

typedef struct {
    Hashjobb *h;
    uint8_t *dp;
    size_t *filnr;        /* jobb -> filnummer */
    uint32_t *jp;         /* (ordoffset, längd) per jobb */
    size_t antal;
} Insamling;

static void samla_en(void *ctx, size_t k) {
    Insamling *in = ctx;
    size_t i = in->filnr[k];
    size_t plats = (size_t)in->jp[k * 2] * 4, n = in->jp[k * 2 + 1];
    Fil f = fil_öppna(in->h->sökvägar[i]);
    if (!f.p) { in->h->hoppat[i] = 1; return; }       /* försvann under oss: cpu får ta den */
    size_t m = f.n < n ? f.n : n;
    if (m) memcpy(in->dp + plats, f.p, m);
    if (m < n) memset(in->dp + plats + m, 0, n - m);
    size_t pad = (4 - (n & 3)) & 3;
    if (pad) memset(in->dp + plats + n, 0, pad);
    fil_stäng(&f);
}

static void storlek_en(void *ctx, size_t i) {
    Hashjobb *h = ctx;
    struct stat st;
    if (stat(h->sökvägar[i], &st) || !S_ISREG(st.st_mode)) { h->hoppat[i] = 1; h->längder[i] = 0; return; }
    h->längder[i] = (uint64_t)st.st_size;
}

static double hasha_gpu(Gpu *g, Hashjobb *h, size_t parti_byte, size_t storfil,
                        size_t *gpu_byte, size_t *gpu_filer, double *kärntid, int trådar) {
    uint32_t grupp = g->subgrupp < 64 ? g->subgrupp : 64;
    size_t max_jobb = 1u << 16;
    Buf data = buf_skapa(g, parti_byte + 1024, 1);
    Buf jobb = buf_skapa(g, max_jobb * 8, 1);
    Buf ut   = buf_skapa(g, max_jobb * 16, 0);          /* cpu läser hem: cachat */
    Rör r = rör_skapa(g, "blake2b.spv", 3, 4, grupp);
    Buf bufar[3] = { data, jobb, ut };
    rör_bind(g, &r, bufar);

    uint8_t *dp = data.p;
    uint32_t *jp = jobb.p;
    uint8_t *up = ut.p;
    size_t *filnr = kalla(max_jobb * sizeof *filnr);

    double t0 = nu();
    *kärntid = 0; *gpu_byte = 0; *gpu_filer = 0;

    /* Storlekarna först, parallellt: de styr hur partierna kan packas. */
    svärm_kör(storlek_en, h, h->antal, trådar);

    size_t i = 0;
    while (i < h->antal) {
        size_t byte = 0, antal = 0;
        while (i < h->antal && antal < max_jobb) {
            if (h->hoppat[i]) { i++; continue; }
            size_t n = (size_t)h->längder[i];
            if (n > storfil) { h->hoppat[i] = 1; i++; continue; }   /* en tråd per fil: för stor */
            size_t plats = (byte + 3) & ~(size_t)3;
            if (plats + n + 4 > parti_byte) break;
            jp[antal * 2]     = (uint32_t)(plats / 4);
            jp[antal * 2 + 1] = (uint32_t)n;
            filnr[antal] = i;
            byte = plats + n + ((4 - (n & 3)) & 3);
            antal++;
            i++;
        }
        if (!antal) continue;
        Insamling in = { .h = h, .dp = dp, .filnr = filnr, .jp = jp, .antal = antal };
        svärm_kör(samla_en, &in, antal, trådar);
        uint32_t push = (uint32_t)antal;
        uint32_t grupper = (uint32_t)((antal + grupp - 1) / grupp);
        *kärntid += rör_kör(g, &r, grupper, &push, sizeof push);
        for (size_t k = 0; k < antal; k++) {
            size_t f = filnr[k];
            if (!h->hoppat[f]) memcpy(&h->hashar[f * 16], up + k * 16, 16);
        }
        *gpu_byte += byte;
        *gpu_filer += antal;
    }
    double dt = nu() - t0;

    /* De överhoppade filerna tas på cpu — annars vore svaret ofullständigt. */
    size_t kvar = 0;
    for (size_t k = 0; k < h->antal; k++) if (h->hoppat[k]) kvar++;
    if (kvar) {
        Hashjobb del = { .sökvägar = kalla(kvar * sizeof(char *)),
                         .hashar = kalla(kvar * 16), .längder = kalla(kvar * 8),
                         .hoppat = kalla(kvar * sizeof(int)), .antal = kvar };
        size_t m = 0;
        for (size_t k = 0; k < h->antal; k++) if (h->hoppat[k]) del.sökvägar[m++] = h->sökvägar[k];
        svärm_kör(hasha_cpu_en, &del, kvar, trådar);
        m = 0;
        for (size_t k = 0; k < h->antal; k++)
            if (h->hoppat[k]) { memcpy(&h->hashar[k * 16], &del.hashar[m * 16], 16);
                                h->längder[k] = del.längder[m]; m++; }
        free(del.sökvägar); free(del.hashar); free(del.längder); free(del.hoppat);
    }

    free(filnr);
    rör_fria(g, &r);
    buf_fria(g, &ut); buf_fria(g, &jobb); buf_fria(g, &data);
    return dt;
}

static Hashjobb läs_sökvägar(void) {
    Hashjobb h = { 0 };
    size_t tak = 1024;
    h.sökvägar = kalla(tak * sizeof(char *));
    char *rad = NULL; size_t cap = 0; ssize_t n;
    while ((n = getline(&rad, &cap, stdin)) > 0) {
        while (n && (rad[n - 1] == '\n' || rad[n - 1] == '\r')) rad[--n] = 0;
        if (!n) continue;
        if (h.antal == tak) { tak *= 2; h.sökvägar = realloc(h.sökvägar, tak * sizeof(char *));
                              if (!h.sökvägar) dö("minnet tog slut"); }
        h.sökvägar[h.antal++] = strdup(rad);
    }
    free(rad);
    h.hashar = kalla(h.antal * 16 + 16);
    h.längder = kalla(h.antal * 8 + 8);
    h.hoppat = kalla(h.antal * sizeof(int) + 4);
    return h;
}

static int kmd_hasha(Gpu *g, int bara_cpu, int trådar, size_t parti, size_t storfil) {
    Hashjobb h = läs_sökvägar();
    if (!h.antal) dö("inga sökvägar på stdin");
    double dt, kärna = 0;
    size_t gbyte = 0, gfiler = 0;
    if (bara_cpu) {
        double t0 = nu();
        svärm_kör(hasha_cpu_en, &h, h.antal, trådar);
        dt = nu() - t0;
    } else {
        dt = hasha_gpu(g, &h, parti, storfil, &gbyte, &gfiler, &kärna, trådar);
    }
    uint64_t summa = 0;
    for (size_t i = 0; i < h.antal; i++) summa += h.längder[i];
    skriv_hashar(&h);
    fprintf(stderr, "hasha %s: %zu filer, %.1f MiB på %.3f s = %.0f MiB/s",
            bara_cpu ? "cpu" : "gpu", h.antal, summa / 1048576.0, dt, summa / 1048576.0 / dt);
    if (!bara_cpu) {
        size_t cpu_filer = h.antal - gfiler;
        fprintf(stderr, " (kärna %.3f s, %zu filer på gpu, %zu för stora och tagna på cpu)",
                kärna, gfiler, cpu_filer);
    } else {
        fprintf(stderr, " (%d trådar)", trådar);
    }
    fputc('\n', stderr);
    return 0;
}

/* --- matcher ------------------------------------------------------------ */

typedef struct { double bygg, cpu, gpu, kopia; uint32_t avvikelser; uint32_t n; } Matchtid;

static Matchtid matcher_mot_cpu(Gpu *g, const uint8_t *data, uint32_t n,
                                uint32_t fönster, uint32_t kedja, uint32_t nog,
                                int trådar, int bara_cpu) {
    Matchtid r = { .n = n };
    Matchning m = { .d = data, .n = n, .fönster = fönster, .kedja = kedja, .nog = nog };
    double t0 = nu();
    bygg_hinkar(&m);
    r.bygg = nu() - t0;

    uint32_t *ut_cpu = kalla((size_t)n * 4 + 4);
    m.ut = ut_cpu;
    r.cpu = matcha_cpu(&m, trådar);

    if (!bara_cpu) {
        uint32_t pos_antal = n > 2 ? n - 2 : 1;
        t0 = nu();
        Buf bdata  = buf_skapa(g, ((size_t)n + 4 + 3) & ~(size_t)3, 1);
        Buf bstart = buf_skapa(g, (HINKAR + 1) * 4, 1);
        Buf bhink  = buf_skapa(g, (size_t)pos_antal * 4, 1);
        Buf bplats = buf_skapa(g, (size_t)n * 4 + 4, 1);
        Buf but    = buf_skapa(g, (size_t)n * 4 + 4, 0);   /* cpu läser hem: cachat */
        memcpy(bdata.p, data, n);
        memset((uint8_t *)bdata.p + n, 0, 4);
        memcpy(bstart.p, m.start, (HINKAR + 1) * 4);
        memcpy(bhink.p, m.hink, (size_t)(n > 2 ? n - 2 : 0) * 4);
        memcpy(bplats.p, m.plats, (size_t)n * 4);
        r.kopia = nu() - t0;

        uint32_t grupp = g->subgrupp < 64 ? g->subgrupp : 64;
        Rör rör = rör_skapa(g, "matcher.spv", 5, 24, grupp);
        Buf bufar[5] = { bdata, bstart, bhink, bplats, but };
        rör_bind(g, &rör, bufar);
        struct { uint32_t n, fönster, kedja, hmask, bas, nog; } push =
            { n, fönster, kedja, HINKAR - 1, 0, nog };
        for (uint32_t bas = 0; bas < n; bas += SKIVA) {
            uint32_t m2 = n - bas < SKIVA ? n - bas : SKIVA;
            push.bas = bas;
            r.gpu += rör_kör(g, &rör, (m2 + grupp - 1) / grupp, &push, sizeof push);
        }

        const uint32_t *ut_gpu = but.p;
        for (uint32_t i = 0; i < n; i++) if (ut_gpu[i] != ut_cpu[i]) r.avvikelser++;

        rör_fria(g, &rör);
        buf_fria(g, &but); buf_fria(g, &bplats); buf_fria(g, &bhink);
        buf_fria(g, &bstart); buf_fria(g, &bdata);
    }

    free(ut_cpu); free(m.start); free(m.hink); free(m.plats);
    return r;
}

static int kmd_matcher(Gpu *g, const char *sökväg, uint32_t fönster, uint32_t kedja,
                       uint32_t nog, int trådar, int bara_cpu, double tak_mib) {
    Fil f = fil_öppna(sökväg);
    if (!f.p) dö("kan inte läsa %s", sökväg);
    uint32_t n = (uint32_t)(f.n < (size_t)(tak_mib * 1048576.0) ? f.n : (size_t)(tak_mib * 1048576.0));
    if (n < 16) dö("%s är för liten (%u byte)", sökväg, n);

    Matchtid r = matcher_mot_cpu(g, f.p, n, fönster, kedja, nog, trådar, bara_cpu);
    double mib = n / 1048576.0;
    säg("matcher: %s, %.1f MiB, fönster %u, kedjetak %u, nog %u\n", sökväg, mib, fönster, kedja, nog);
    säg("  hinkbygge (cpu, båda vägarna behöver det)  %7.3f s  %8.0f MiB/s\n", r.bygg, mib / r.bygg);
    säg("  cpu-sökning (%2d trådar)                    %7.3f s  %8.0f MiB/s\n", trådar, r.cpu, mib / r.cpu);
    if (!bara_cpu) {
        säg("  gpu-kopiering in                           %7.3f s  %8.0f MiB/s\n", r.kopia, mib / r.kopia);
        säg("  gpu-sökning                                %7.3f s  %8.0f MiB/s\n", r.gpu, mib / r.gpu);
        säg("  gpu/cpu bara sökning   %.2fx\n", r.cpu / r.gpu);
        säg("  gpu/cpu med kopiering  %.2fx\n", r.cpu / (r.gpu + r.kopia));
        säg("  avvikelser mot cpu     %u av %u positioner%s\n", r.avvikelser, r.n,
            r.avvikelser ? "  ← FEL, tiderna betyder inget" : "  (identiska)");
    }
    fil_stäng(&f);
    return r.avvikelser ? 1 : 0;
}

/* --- prov --------------------------------------------------------------- */

static int hex_lika(const uint8_t h[16], const char *vänta) {
    char hex[33];
    for (int k = 0; k < 16; k++) snprintf(hex + k * 2, 3, "%02x", h[k]);
    return strcmp(hex, vänta) == 0;
}

/* blake2b-128 av kända indata, hämtade ur hashlib (pythondelens egen
 * implementation). Stämmer inte dessa är hela gpu-läget meningslöst, för då
 * är det inte samma hash som arkiven är byggda med. */
static const struct { size_t n; const char *mönster; const char *vänta; } VEKTOR[] = {
    { 0,    "",    "cae66941d9efbd404e4d88758ea67670" },
    { 3,    "abc", "cf4ab791c62b8d2b2109c90275287816" },
    { 128,  "a",   "1271d28e731afb57bdfba6bdaf3501be" },
    { 129,  "a",   "68b7bafbad5055c4ebe6eb170a0f8524" },
};

static int kmd_prov(Gpu *g, int trådar) {
    int fel = 0;
    säg("prov: blake2b-128 på cpu mot kända värden\n");
    for (size_t i = 0; i < sizeof VEKTOR / sizeof *VEKTOR; i++) {
        uint8_t *b = kalla(VEKTOR[i].n + 1);
        size_t ml = strlen(VEKTOR[i].mönster);
        for (size_t k = 0; k < VEKTOR[i].n; k++) b[k] = (uint8_t)VEKTOR[i].mönster[ml ? k % ml : 0];
        uint8_t h[16];
        blake2b128(b, VEKTOR[i].n, h);
        int ok = hex_lika(h, VEKTOR[i].vänta);
        säg("  %4zu byte  %s\n", VEKTOR[i].n, ok ? "ok" : "FEL");
        if (!ok) fel++;
        free(b);
    }
    /* 256 byte 0..255 upprepat fyra gånger — täcker alla bytevärden. */
    {
        uint8_t b[1024];
        for (int i = 0; i < 1024; i++) b[i] = (uint8_t)(i & 255);
        uint8_t h[16];
        blake2b128(b, 1024, h);
        int ok = hex_lika(h, "0b4661484ed1cc8e28c4a7f18e8b1019");
        säg("  1024 byte  %s\n", ok ? "ok" : "FEL");
        if (!ok) fel++;
    }

    /* GPU mot CPU över längder som bråkar: tomt, ordgränser, 128-gränsen
     * (blake2b:s sista block får aldrig vara tomt om meddelandet inte är det)
     * och ett par stora. */
    säg("prov: blake2b-128 gpu mot cpu över %d längder\n", 23);
    static const uint32_t längder[] = { 0,1,2,3,4,5,7,8,63,64,65,127,128,129,
                                        255,256,257,1000,4096,65536,131071,131072,1048577 };
    size_t antal = sizeof längder / sizeof *längder;
    uint32_t grupp = g->subgrupp < 64 ? g->subgrupp : 64;
    size_t total = 0;
    for (size_t i = 0; i < antal; i++) total += (längder[i] + 3) & ~3u;
    Buf data = buf_skapa(g, total + 1024, 1);
    Buf jobb = buf_skapa(g, antal * 8, 1);
    Buf ut   = buf_skapa(g, antal * 16, 1);
    Rör r = rör_skapa(g, "blake2b.spv", 3, 4, grupp);
    Buf bufar[3] = { data, jobb, ut };
    rör_bind(g, &r, bufar);
    uint8_t *dp = data.p; uint32_t *jp = jobb.p;
    size_t plats = 0;
    uint8_t **kopior = kalla(antal * sizeof(uint8_t *));
    for (size_t i = 0; i < antal; i++) {
        uint32_t n = längder[i];
        kopior[i] = kalla(n + 1);
        for (uint32_t k = 0; k < n; k++) kopior[i][k] = (uint8_t)((k * 37 + i * 11) & 255);
        memcpy(dp + plats, kopior[i], n);
        size_t pad = (4 - (n & 3)) & 3;
        if (pad) memset(dp + plats + n, 0, pad);
        jp[i * 2] = (uint32_t)(plats / 4);
        jp[i * 2 + 1] = n;
        plats += n + pad;
    }
    uint32_t push = (uint32_t)antal;
    rör_kör(g, &r, (uint32_t)((antal + grupp - 1) / grupp), &push, sizeof push);
    const uint8_t *up = ut.p;
    for (size_t i = 0; i < antal; i++) {
        uint8_t h[16];
        blake2b128(kopior[i], längder[i], h);
        int ok = memcmp(h, up + i * 16, 16) == 0;
        if (!ok) {
            fel++;
            säg("  %8u byte  FEL\n", längder[i]);
        }
        free(kopior[i]);
    }
    free(kopior);
    säg("  %s\n", fel ? "avvikelser finns" : "alla identiska med cpu");
    rör_fria(g, &r);
    buf_fria(g, &ut); buf_fria(g, &jobb); buf_fria(g, &data);

    /* Matcharen: gpu mot cpu på data med både upprepningar och brus. */
    säg("prov: lz77-matchning gpu mot cpu\n");
    uint32_t n = 4u << 20;
    uint8_t *b = kalla(n);
    const char *bit = "void main(void) { return nvpack(0x1234, \"kontext\"); }\n";
    size_t bl = strlen(bit);
    uint32_t frö = 12345;
    for (uint32_t i = 0; i < n; ) {
        frö = frö * 1103515245u + 12345u;
        if ((frö >> 16) % 3) {                       /* två tredjedelar text */
            size_t t = bl < n - i ? bl : n - i;
            memcpy(b + i, bit, t); i += (uint32_t)t;
        } else {                                      /* en tredjedel brus */
            uint32_t t = (frö >> 8) % 64 + 1;
            for (uint32_t k = 0; k < t && i < n; k++, i++) {
                frö = frö * 1103515245u + 12345u;
                b[i] = (uint8_t)(frö >> 24);
            }
        }
    }
    for (int varv = 0; varv < 2; varv++) {
        uint32_t kedja = varv ? 4096 : 32;
        Matchtid r2 = matcher_mot_cpu(g, b, n, 32768, kedja, 258, trådar, 0);
        säg("  kedjetak %-5u avvikelser %u av %u  (cpu %.3f s, gpu %.3f s)\n",
            kedja, r2.avvikelser, r2.n, r2.cpu, r2.gpu);
        if (r2.avvikelser) fel++;
    }
    free(b);

    säg("%s\n", fel ? "PROV MISSLYCKADES" : "alla prov gick igenom");
    return fel ? 1 : 0;
}

/* ----------------------------------------------------------------------- */
/* nvlz — kodeken som faktiskt använder gpu:n                              */
/* ----------------------------------------------------------------------- */
/* Matchningssökningen är det enda i en packare som är brett parallellt, och
 * mätningen ovan säger att gpu:n tar den fyra till sex gånger snabbare än 24
 * cpu-trådar. Men zlib, xz och zstd kan inte TA EMOT färdiga matchningar —
 * deras utdata måste bli bit för bit vad deras egna sökningar hade gett.
 * Därför finns den här kodeken: nvpacks eget format, där den dyra halvan
 * (sökningen) ligger på gpu:n och den seriella halvan (tokenvalet och
 * entropikodningen) på cpu:n.
 *
 * UPPACKNINGEN ÄR REN CPU. Ett arkiv som packats med nvlz måste gå att öppna
 * på en maskin utan vulkan, annars är arkivet en gisslan hos en drivrutin.
 *
 * Formatet: "NVLZ1", sedan stycken om högst STYCKE byte rådata. Varje stycke
 * har rålängd, kodad längd och en flagga för rålagring (när kodningen inte
 * tjänade något). Matchningar pekar bara inom sitt eget stycke — det kostar
 * någon tiondels procent vid styckgränserna och gör uppackaren enkel nog att
 * lita på.
 */

#define STYCKE   (32u << 20)
#define LZFONSTER (1u << 22)          /* 4 MiB bakåt; avståndet ryms i 23 bitar */
#define PROB_BIT 11
#define PROB_EN  (1u << PROB_BIT)
#define PROB_FLYTT 5

typedef uint16_t Prob;

/* Modellerna. Tre val som mätningen drev fram (de kostade 2,1x -> 4,3x på
 * kodkorpusen):
 *
 *   litteral[föregående byte][...]  — ORDER-1. Sexton kontexter (den övre
 *     nibbeln) räckte inte alls: i text avgörs nästa tecken av tecknet före,
 *     och 'q' följt av 'u' är inte samma fördelning som ' ' följt av 'u'.
 *   rep — "samma avstånd som förra matchningen". I strukturerad text
 *     (json, loggar, källkod med upprepade rader) är det vanligaste
 *     avståndet det man just använde, och då är en bit billigare än en
 *     avståndskod.
 *   langd[är rep][...] — repmatchningar har en annan längdfördelning än
 *     friska matchningar, så de får egna träd. */
typedef struct {
    Prob matchning[2][16];
    Prob rep[2][16];
    Prob litteral[256][256];
    Prob langd[2][256];
    Prob slot[32];
    Prob mitt[24][256];
} Modeller;

static void modeller_noll(Modeller *m) {
    Prob *p = (Prob *)m;
    size_t n = sizeof *m / sizeof(Prob);
    for (size_t i = 0; i < n; i++) p[i] = PROB_EN / 2;
}

/* --- intervallkodaren (lzma-sorten) ------------------------------------- */

typedef struct {
    uint8_t *ut; size_t n, tak;
    uint64_t lag; uint32_t intervall; uint8_t cache; uint64_t cacheantal;
} Kod;

static void kod_byte(Kod *k, uint8_t b) {
    if (k->n == k->tak) {
        k->tak = k->tak ? k->tak * 2 : 1 << 16;
        k->ut = realloc(k->ut, k->tak);
        if (!k->ut) dö("minnet tog slut i kodaren");
    }
    k->ut[k->n++] = b;
}

static void kod_start(Kod *k) {
    k->ut = NULL; k->n = 0; k->tak = 0;
    k->lag = 0; k->intervall = 0xFFFFFFFFu; k->cache = 0; k->cacheantal = 1;
}

static void kod_skifta(Kod *k) {
    if ((uint32_t)(k->lag >> 32) != 0 || (uint32_t)k->lag < 0xFF000000u) {
        uint8_t t = k->cache;
        do { kod_byte(k, (uint8_t)(t + (uint8_t)(k->lag >> 32))); t = 0xFF; } while (--k->cacheantal);
        k->cache = (uint8_t)((uint32_t)k->lag >> 24);
    }
    k->cacheantal++;
    k->lag = (uint64_t)((uint32_t)k->lag << 8);
}

static void kod_bit(Kod *k, Prob *p, int bit) {
    uint32_t grans = (k->intervall >> PROB_BIT) * *p;
    if (!bit) { k->intervall = grans; *p = (Prob)(*p + ((PROB_EN - *p) >> PROB_FLYTT)); }
    else { k->lag += grans; k->intervall -= grans; *p = (Prob)(*p - (*p >> PROB_FLYTT)); }
    while (k->intervall < (1u << 24)) { k->intervall <<= 8; kod_skifta(k); }
}

static void kod_raka(Kod *k, uint32_t v, int bitar) {
    for (int i = bitar - 1; i >= 0; i--) {
        k->intervall >>= 1;
        if ((v >> i) & 1) k->lag += k->intervall;
        while (k->intervall < (1u << 24)) { k->intervall <<= 8; kod_skifta(k); }
    }
}

static void kod_slut(Kod *k) { for (int i = 0; i < 5; i++) kod_skifta(k); }

/* Bitträd: `bitar` bitar, modell per nod. Samma ordning i kodare och
 * avkodare — det är hela kontraktet mellan dem. */
static void kod_trad(Kod *k, Prob *träd, uint32_t v, int bitar) {
    uint32_t nod = 1;
    for (int i = bitar - 1; i >= 0; i--) {
        int bit = (v >> i) & 1;
        kod_bit(k, &träd[nod], bit);
        nod = (nod << 1) | (uint32_t)bit;
    }
}

typedef struct {
    const uint8_t *in; size_t n, i;
    uint32_t intervall, kod;
} Avkod;

static uint8_t avkod_byte(Avkod *a) { return a->i < a->n ? a->in[a->i++] : 0; }

static void avkod_start(Avkod *a, const uint8_t *in, size_t n) {
    a->in = in; a->n = n; a->i = 0;
    a->intervall = 0xFFFFFFFFu; a->kod = 0;
    avkod_byte(a);                                  /* kodarens första byte är alltid 0 */
    for (int i = 0; i < 4; i++) a->kod = (a->kod << 8) | avkod_byte(a);
}

static int avkod_bit(Avkod *a, Prob *p) {
    uint32_t grans = (a->intervall >> PROB_BIT) * *p;
    int bit;
    if (a->kod < grans) { a->intervall = grans; *p = (Prob)(*p + ((PROB_EN - *p) >> PROB_FLYTT)); bit = 0; }
    else { a->kod -= grans; a->intervall -= grans; *p = (Prob)(*p - (*p >> PROB_FLYTT)); bit = 1; }
    while (a->intervall < (1u << 24)) { a->intervall <<= 8; a->kod = (a->kod << 8) | avkod_byte(a); }
    return bit;
}

static uint32_t avkod_raka(Avkod *a, int bitar) {
    uint32_t v = 0;
    for (int i = 0; i < bitar; i++) {
        a->intervall >>= 1;
        int bit = 0;
        if (a->kod >= a->intervall) { a->kod -= a->intervall; bit = 1; }
        while (a->intervall < (1u << 24)) { a->intervall <<= 8; a->kod = (a->kod << 8) | avkod_byte(a); }
        v = (v << 1) | (uint32_t)bit;
    }
    return v;
}

static uint32_t avkod_trad(Avkod *a, Prob *träd, int bitar) {
    uint32_t nod = 1;
    for (int i = 0; i < bitar; i++) nod = (nod << 1) | (uint32_t)avkod_bit(a, &träd[nod]);
    return nod - (1u << bitar);
}

static int bitlangd(uint32_t v) { int n = 0; while (v) { n++; v >>= 1; } return n; }

/* --- kodning av ett stycke ---------------------------------------------- */

/* Avståndet: först hur många bitar det är (slot), sedan bitarna under den
 * ledande ettan. De åtta översta av dem får modeller — där sitter mönstret,
 * för avstånden i riktig data klumpar sig. Resten kodas rakt. */
static void kod_avstand(Kod *k, Modeller *m, uint32_t dist) {
    uint32_t v = dist - 1;
    int nb = bitlangd(v);
    kod_trad(k, m->slot, (uint32_t)nb, 5);
    if (nb >= 2) {
        int kvar = nb - 1;
        uint32_t under = v & ((1u << kvar) - 1);
        if (kvar <= 8) kod_trad(k, m->mitt[nb], under, kvar);
        else {
            kod_trad(k, m->mitt[nb], under >> (kvar - 8), 8);
            kod_raka(k, under & ((1u << (kvar - 8)) - 1), kvar - 8);
        }
    }
}

static uint32_t avkod_avstand(Avkod *a, Modeller *m) {
    int nb = (int)avkod_trad(a, m->slot, 5);
    uint32_t v;
    if (nb == 0) v = 0;
    else if (nb == 1) v = 1;
    else {
        int kvar = nb - 1;
        uint32_t under;
        if (kvar <= 8) under = avkod_trad(a, m->mitt[nb], kvar);
        else {
            under = avkod_trad(a, m->mitt[nb], 8) << (kvar - 8);
            under |= avkod_raka(a, kvar - 8);
        }
        v = (1u << kvar) | under;
    }
    return v + 1;
}

/* Lata valet: en matchning som genast följs av en längre är inte värd att ta.
 * Det är zlibs `max_lazy`-tanke, och den ger ett par procent gratis. */
static size_t koda_stycke(const uint8_t *d, uint32_t n, const uint32_t *mt, Kod *k) {
    Modeller *m = kalla(sizeof *m);
    modeller_noll(m);
    kod_start(k);
    uint32_t pos = 0;
    uint8_t forra = 0;
    int forra_var_match = 0;
    uint32_t rep0 = 0;
    while (pos < n) {
        uint32_t a = mt[pos];
        uint32_t len = a >> 23, dist = a & 0x7FFFFFu;
        if (len >= 3 && pos + 1 < n) {
            uint32_t b = mt[pos + 1];
            if ((b >> 23) > len) len = 0;          /* nästa position tar hem mer */
        }
        Prob *pm = &m->matchning[forra_var_match][forra >> 4];
        if (len >= 3) {
            kod_bit(k, pm, 1);
            int ar_rep = (rep0 && dist == rep0);
            kod_bit(k, &m->rep[forra_var_match][forra >> 4], ar_rep);
            kod_trad(k, m->langd[ar_rep], len - 3, 8);
            if (!ar_rep) { kod_avstand(k, m, dist); rep0 = dist; }
            pos += len;
            forra = d[pos - 1];
            forra_var_match = 1;
        } else {
            kod_bit(k, pm, 0);
            kod_trad(k, m->litteral[forra], d[pos], 8);
            forra = d[pos];
            pos++;
            forra_var_match = 0;
        }
    }
    kod_slut(k);
    free(m);
    return k->n;
}

static int avkoda_stycke(const uint8_t *in, size_t inn, uint8_t *ut, uint32_t n) {
    Modeller *m = kalla(sizeof *m);
    modeller_noll(m);
    Avkod a;
    avkod_start(&a, in, inn);
    uint32_t pos = 0;
    uint8_t forra = 0;
    int forra_var_match = 0;
    uint32_t rep0 = 0;
    while (pos < n) {
        Prob *pm = &m->matchning[forra_var_match][forra >> 4];
        if (avkod_bit(&a, pm)) {
            int ar_rep = avkod_bit(&a, &m->rep[forra_var_match][forra >> 4]);
            uint32_t len = avkod_trad(&a, m->langd[ar_rep], 8) + 3;
            uint32_t dist;
            if (ar_rep) dist = rep0;
            else { dist = avkod_avstand(&a, m); rep0 = dist; }
            if (!dist || dist > pos || len > n - pos) { free(m); return -1; }   /* trasig ström */
            for (uint32_t i = 0; i < len; i++) ut[pos + i] = ut[pos + i - dist];
            pos += len;
            forra = ut[pos - 1];
            forra_var_match = 1;
        } else {
            uint8_t b = (uint8_t)avkod_trad(&a, m->litteral[forra], 8);
            ut[pos++] = b;
            forra = b;
            forra_var_match = 0;
        }
    }
    free(m);
    return 0;
}

/* --- matchningar för ett stycke: gpu om den finns, annars cpu ------------ */

typedef struct {
    Gpu *g;
    Rör rör;
    Buf data, start, hink, plats, ut;
    uint32_t tak;            /* styckets maxstorlek */
    uint32_t grupp;
    int redo;
} Matchare;

static void matchare_oppna(Matchare *mm, Gpu *g, uint32_t tak) {
    memset(mm, 0, sizeof *mm);
    mm->g = g; mm->tak = tak;
    if (!g) return;
    mm->grupp = g->subgrupp < 64 ? g->subgrupp : 64;
    mm->data  = buf_skapa(g, ((size_t)tak + 8), 1);
    mm->start = buf_skapa(g, (HINKAR + 1) * 4, 1);
    mm->hink  = buf_skapa(g, (size_t)tak * 4, 1);
    mm->plats = buf_skapa(g, (size_t)tak * 4 + 4, 1);
    mm->ut    = buf_skapa(g, (size_t)tak * 4 + 4, 0);   /* cpu läser hem: cachat */
    mm->rör = rör_skapa(g, "matcher.spv", 5, 16, mm->grupp);
    Buf bufar[5] = { mm->data, mm->start, mm->hink, mm->plats, mm->ut };
    rör_bind(g, &mm->rör, bufar);
    mm->redo = 1;
}

static void matchare_stang(Matchare *mm) {
    if (!mm->redo) return;
    rör_fria(mm->g, &mm->rör);
    buf_fria(mm->g, &mm->ut); buf_fria(mm->g, &mm->plats); buf_fria(mm->g, &mm->hink);
    buf_fria(mm->g, &mm->start); buf_fria(mm->g, &mm->data);
    mm->redo = 0;
}

/* Fyller `ut` med len<<23|dist per position. */
static void matchare_kor(Matchare *mm, const uint8_t *d, uint32_t n, uint32_t fönster,
                         uint32_t kedja, uint32_t nog, uint32_t *ut, int trådar, double *gpu_tid) {
    Matchning m = { .d = d, .n = n, .fönster = fönster, .kedja = kedja, .nog = nog, .ut = ut };
    bygg_hinkar(&m);
    if (mm->redo) {
        memcpy(mm->data.p, d, n);
        memset((uint8_t *)mm->data.p + n, 0, 8);
        memcpy(mm->start.p, m.start, (HINKAR + 1) * 4);
        if (n > 2) memcpy(mm->hink.p, m.hink, (size_t)(n - 2) * 4);
        memcpy(mm->plats.p, m.plats, (size_t)n * 4);
        struct { uint32_t n, fönster, kedja, hmask, bas, nog; } push =
            { n, fönster, kedja, HINKAR - 1, 0, nog };
        for (uint32_t bas = 0; bas < n; bas += SKIVA) {
            uint32_t m2 = n - bas < SKIVA ? n - bas : SKIVA;
            push.bas = bas;
            *gpu_tid += rör_kör(mm->g, &mm->rör, (m2 + mm->grupp - 1) / mm->grupp, &push, sizeof push);
        }
        memcpy(ut, mm->ut.p, (size_t)n * 4);
    } else {
        matcha_cpu(&m, trådar);
    }
    free(m.start); free(m.hink); free(m.plats);
}

/* --- packa och packa upp på stdin/stdout -------------------------------- */

static const char NVLZ_MAGI[5] = { 'N', 'V', 'L', 'Z', '1' };

static void skriv_allt(const void *p, size_t n) {
    if (n && fwrite(p, 1, n, stdout) != n) dö("kunde inte skriva till stdout");
}

static size_t las_allt(uint8_t **ut) {
    size_t tak = 1u << 22, n = 0;
    uint8_t *b = kalla(tak);
    for (;;) {
        if (n == tak) { tak *= 2; b = realloc(b, tak); if (!b) dö("minnet tog slut"); }
        size_t r = fread(b + n, 1, tak - n, stdin);
        n += r;
        if (r == 0) {
            if (ferror(stdin)) dö("kunde inte läsa från stdin");
            break;                       /* filslut, inte fel */
        }
    }
    *ut = b;
    return n;
}

static int kmd_packa(Gpu *g, uint32_t fönster, uint32_t kedja, uint32_t nog, int trådar) {
    uint8_t *rå = NULL;
    size_t n = las_allt(&rå);
    skriv_allt(NVLZ_MAGI, sizeof NVLZ_MAGI);

    uint32_t stycke = STYCKE;
    Matchare mm;
    matchare_oppna(&mm, g, n < stycke ? (uint32_t)(n ? n : 1) : stycke);
    uint32_t *mt = kalla(((size_t)(n < stycke ? n : stycke) + 1) * 4);
    double gpu_tid = 0;
    size_t skrivet = 0;

    for (size_t off = 0; off < n || off == 0; off += stycke) {
        uint32_t m = (uint32_t)(n - off < stycke ? n - off : stycke);
        matchare_kor(&mm, rå + off, m, fönster, kedja, nog, mt, trådar, &gpu_tid);
        Kod k;
        size_t kn = koda_stycke(rå + off, m, mt, &k);
        uint8_t huvud[9];
        int rått = kn >= m;                       /* kodningen tjänade inget */
        huvud[0] = (uint8_t)(rått ? 1 : 0);
        uint32_t a = m, b = (uint32_t)(rått ? m : kn);
        memcpy(huvud + 1, &a, 4);
        memcpy(huvud + 5, &b, 4);
        skriv_allt(huvud, 9);
        skriv_allt(rått ? rå + off : k.ut, rått ? m : kn);
        skrivet += 9 + (rått ? m : kn);
        free(k.ut);
        if (n - off <= stycke) break;
    }
    fflush(stdout);
    free(mt); free(rå);
    matchare_stang(&mm);
    if (!TYST && g)
        fprintf(stderr, "nvlz: %.2f MiB -> %.2f MiB (x%.2f), gpu-sökning %.3f s\n",
                n / 1048576.0, (skrivet + 5) / 1048576.0,
                (double)n / (double)(skrivet + 5), gpu_tid);
    return 0;
}

static int kmd_packa_upp(void) {
    uint8_t *in = NULL;
    size_t n = las_allt(&in);
    if (n < 5 || memcmp(in, NVLZ_MAGI, 5)) dö("inte en nvlz-ström");
    size_t i = 5;
    while (i < n) {
        if (i + 9 > n) dö("avhuggen ström (styckhuvud)");
        int rått = in[i];
        uint32_t m, kn;
        memcpy(&m, in + i + 1, 4);
        memcpy(&kn, in + i + 5, 4);
        i += 9;
        if (i + kn > n) dö("avhuggen ström (stycke)");
        if (rått) {
            skriv_allt(in + i, kn);
        } else {
            uint8_t *ut = kalla(m ? m : 1);
            if (avkoda_stycke(in + i, kn, ut, m)) dö("trasigt stycke");
            skriv_allt(ut, m);
            free(ut);
        }
        i += kn;
    }
    fflush(stdout);
    free(in);
    return 0;
}

/* ----------------------------------------------------------------------- */
/* Kommandorad                                                             */
/* ----------------------------------------------------------------------- */

static void hjälp(void) {
    puts("nvgpu — gpu-läget i nvpack (vulkan-compute)\n"
         "\n"
         "  nvgpu enhet                 vilken enhet som valdes och vad den klarar\n"
         "  nvgpu band [MiB]            bandbreddstak: gpu-läsning mot cpu-läsning\n"
         "  nvgpu hasha < sökvägar      blake2b-128 per fil, bitexakt mot hashlib\n"
         "  nvgpu matcher FIL           lz77-längsta-matchning: gpu mot cpu, bit för bit\n"
         "  nvgpu prov                  självprov: kända testvektorer + gpu==cpu\n"
         "  nvgpu packa < rå > nv       kodeken nvlz: lz77 på gpu, kodning på cpu\n"
         "  nvgpu packaupp < nv > rå    uppackning (ren cpu, behöver ingen gpu)\n"
         "\n"
         "flaggor:\n"
         "  --cpu           kör bara cpu-vägen (för jämförelsen)\n"
         "  --jobb N        cpu-trådar (standard: alla)\n"
         "  --fonster N     matcharens fönster i byte (standard 32768)\n"
         "  --kedja N       hur många kandidater matcharen provar (standard 4096)\n"
         "  --nog N         en matchning så lång räcker, sluta leta (standard 258)\n"
         "  --storfil MiB   filer större än detta hashas på cpu (standard 16)\n"
         "  --parti MiB     gpu-buffertens storlek per parti (standard 192)\n"
         "  --tak MiB       läs högst så mycket av filen i `matcher` (standard 64)\n"
         "  --tyst          bara fel på utdata\n"
         "\n"
         "NVGPU_ENHET=n väljer vulkan-enhet n i stället för automatiskt.");
}

int main(int argc, char **argv) {
    if (argc < 2 || !strcmp(argv[1], "--hjälp") || !strcmp(argv[1], "-h")) { hjälp(); return argc < 2 ? 2 : 0; }

    const char *kmd = argv[1];
    int bara_cpu = 0;
    int trådar = (int)sysconf(_SC_NPROCESSORS_ONLN);
    uint32_t fönster = 32768, kedja = 4096, nog = 258, lz_fönster = 0;
    /* `matcher` mäter och ska därför gå till botten med varje kedja. `packa`
     * ska packa, och där är standardvärdena de MÄTTA: kedja 64 och nog 96 på
     * ett fyramegabytesfönster gav x8,0 på kodkorpusen, medan kedja 4096 gav
     * x8,3 för tre gånger tiden. Flaggorna slår över dem. */
    int kedja_satt = 0, nog_satt = 0;
    double storfil = 16, parti = 192, tak = 64, bandmib = 512;
    const char *fil = NULL;

    for (int i = 2; i < argc; i++) {
        const char *a = argv[i];
        int har_värde = i + 1 < argc;
        if (!strcmp(a, "--cpu")) bara_cpu = 1;
        else if (!strcmp(a, "--tyst")) TYST = 1;
        else if (!strcmp(a, "--jobb") && har_värde) trådar = atoi(argv[++i]);
        else if (!strcmp(a, "--fonster") && har_värde) fönster = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--kedja") && har_värde) { kedja = (uint32_t)strtoul(argv[++i], NULL, 10); kedja_satt = 1; }
        else if (!strcmp(a, "--nog") && har_värde) { nog = (uint32_t)strtoul(argv[++i], NULL, 10); nog_satt = 1; }
        else if (!strcmp(a, "--lzfonster") && har_värde) lz_fönster = (uint32_t)strtoul(argv[++i], NULL, 10);
        else if (!strcmp(a, "--storfil") && har_värde) storfil = atof(argv[++i]);
        else if (!strcmp(a, "--parti") && har_värde) parti = atof(argv[++i]);
        else if (!strcmp(a, "--tak") && har_värde) tak = atof(argv[++i]);
        else if (a[0] == '-') dö("okänd flagga %s (se --hjälp)", a);
        else if (!fil) fil = a;
        else dö("för många argument: %s", a);
    }
    if (trådar < 1) trådar = 1;
    if (fönster > (1u << 23) - 1) dö("--fonster får vara högst %u", (1u << 23) - 1);

    if (!strcmp(kmd, "band") && fil) bandmib = atof(fil);

    /* --cpu ska fungera även på en maskin utan gpu alls. */
    Gpu g;
    int behöver_gpu = strcmp(kmd, "packaupp") != 0
        && !(bara_cpu && (!strcmp(kmd, "hasha") || !strcmp(kmd, "matcher") || !strcmp(kmd, "packa")));
    if (behöver_gpu) gpu_öppna(&g);

    int ut;
    if (!strcmp(kmd, "enhet")) ut = kmd_enhet(&g);
    else if (!strcmp(kmd, "band")) ut = kmd_band(&g, bandmib, trådar);
    else if (!strcmp(kmd, "hasha")) ut = kmd_hasha(behöver_gpu ? &g : NULL, bara_cpu, trådar,
                                                   (size_t)(parti * 1048576.0), (size_t)(storfil * 1048576.0));
    else if (!strcmp(kmd, "matcher")) {
        if (!fil) dö("matcher behöver en fil");
        ut = kmd_matcher(behöver_gpu ? &g : NULL, fil, fönster, kedja, nog, trådar, bara_cpu, tak);
    }
    else if (!strcmp(kmd, "prov")) ut = kmd_prov(&g, trådar);
    else if (!strcmp(kmd, "packa"))
        ut = kmd_packa(behöver_gpu ? &g : NULL, lz_fönster ? lz_fönster : LZFONSTER,
                       kedja_satt ? kedja : 64, nog_satt ? nog : 96, trådar);
    else if (!strcmp(kmd, "packaupp")) ut = kmd_packa_upp();
    else { hjälp(); return 2; }

    if (behöver_gpu) gpu_stäng(&g);
    return ut;
}
