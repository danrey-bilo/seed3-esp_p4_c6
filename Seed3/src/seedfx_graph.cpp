#include "seedfx_graph.h"
#include "seedfx_extensions.h"
#include "seedfx_ports.h"
#include "seedfx_routing.h"

#include "dev/sdram.h"

#include <cmath>
#include <cstring>

namespace
{
constexpr std::size_t kChannels = 2U;
constexpr std::size_t kMaxFrames = 32U;
constexpr std::size_t kArenaBytes = 62U * 1024U * 1024U;
constexpr std::size_t kDelayFrames = 96000U;
constexpr std::size_t kDelayBankBytes = SEEDFX_MAX_NODES * kChannels
                                        * kDelayFrames * sizeof(float);
constexpr std::size_t kRuntimeBytes = 2U * kDelayBankBytes;
constexpr std::size_t kCacheLimit = kArenaBytes - kRuntimeBytes;
constexpr uint32_t kArenaMagic = 0x31414653U; // "SFA1"
constexpr float kPi = 3.14159265358979323846f;

struct RuntimeNode
{
    SeedFxNodeDefinition definition;
    float parameter_cache[SEEDFX_MAX_PARAMS];
    float parameter_target[SEEDFX_MAX_PARAMS];
    float state[kChannels][8];
    float hold[kChannels];
    float phase;
    uint32_t hold_counter;
    uint32_t write_index;
    uint32_t valid_frames;
};

struct RuntimeGraph
{
    SeedFxGraphDefinition definition;
    RuntimeNode nodes[SEEDFX_MAX_NODES];
    uint8_t order[SEEDFX_MAX_NODES];
    uint8_t order_count;
    uint16_t needed_by_output[kChannels];
};

struct ArenaHeader
{
    uint32_t magic;
    uint32_t bytes_used;
    uint32_t package_count;
    uint32_t reserved;
};

struct UploadState
{
    bool active;
    uint32_t package_id;
    uint32_t start;
    uint32_t total_bytes;
    uint32_t written;
    uint32_t expected_crc;
    uint32_t running_crc;
};

/* Lower SDRAM is the verified package cache. Two fixed delay banks live at
 * the top, one per hot-swappable graph. Audio processing never allocates. */
alignas(32) uint8_t DSY_SDRAM_BSS g_arena[kArenaBytes];
RuntimeGraph g_graphs[2];
float g_node_audio[SEEDFX_MAX_NODES][kChannels][kMaxFrames];
volatile uint32_t g_active_graph;
volatile uint32_t g_pending_graph = UINT32_MAX;
uint32_t g_graph_commits;
uint32_t g_graph_rejects;
UploadState g_upload{};

ArenaHeader& Arena() { return *reinterpret_cast<ArenaHeader*>(g_arena); }

float Clamp(float value, float low, float high)
{
    return value < low ? low : (value > high ? high : value);
}

float SoftClip(float value) { return value / (1.0f + std::fabs(value)); }

float Triangle(float phase)
{
    return 1.0f - 4.0f * std::fabs(phase - 0.5f);
}

void AdvancePhase(RuntimeNode& node, float hz, uint32_t sample_rate)
{
    node.phase += hz / static_cast<float>(sample_rate);
    if(node.phase >= 1.0f) node.phase -= 1.0f;
}

float* DelayBuffer(uint32_t graph_slot, uint8_t node, uint8_t channel)
{
    const std::size_t bank = kCacheLimit + graph_slot * kDelayBankBytes;
    const std::size_t offset = (static_cast<std::size_t>(node) * kChannels
                                + channel) * kDelayFrames * sizeof(float);
    return reinterpret_cast<float*>(g_arena + bank + offset);
}

float DelayRead(const RuntimeNode& node, const float* buffer, uint32_t delay)
{
    delay = delay < 1U ? 1U : (delay >= kDelayFrames
        ? kDelayFrames - 1U : delay);
    if(node.valid_frames < delay) return 0.0f;
    const uint32_t index = node.write_index >= delay
        ? node.write_index - delay : node.write_index + kDelayFrames - delay;
    return buffer[index];
}

bool EffectTypeValid(uint16_t type)
{
    return type >= SEEDFX_EFFECT_BYPASS && type <= SEEDFX_EFFECT_LAST;
}

int FindNode(const SeedFxGraphDefinition& graph, uint16_t id)
{
    for(uint8_t i = 0; i < graph.node_count; ++i)
        if(graph.nodes[i].id == id) return i;
    return -1;
}

SeedFxGraphResult BuildRuntime(const SeedFxGraphDefinition& source,
                               RuntimeGraph& destination)
{
    if(!seedfx_graph_header_is_valid(&source))
        return SeedFxGraphResult::InvalidHeader;
    if(seedfx_graph_crc32(&source) != source.crc32)
        return SeedFxGraphResult::InvalidCrc;
    if(!seedfx_routes_valid(&source)) return SeedFxGraphResult::InvalidEdge;

    uint8_t indegree[SEEDFX_MAX_NODES]{};
    for(uint8_t i = 0; i < source.node_count; ++i)
    {
        const SeedFxNodeDefinition& node = source.nodes[i];
        if(node.id == SEEDFX_ENDPOINT_AUDIO || !EffectTypeValid(node.effect_type)
           || node.parameter_count > SEEDFX_MAX_PARAMS)
            return SeedFxGraphResult::InvalidNode;
        for(uint8_t p = 0; p < node.parameter_count; ++p)
            if(!std::isfinite(node.parameters[p]))
                return SeedFxGraphResult::InvalidNode;
        for(uint8_t previous = 0; previous < i; ++previous)
            if(source.nodes[previous].id == node.id)
                return SeedFxGraphResult::InvalidNode;
    }
    for(uint8_t i = 0; i < source.edge_count; ++i)
    {
        const SeedFxEdgeDefinition& edge = source.edges[i];
        if(edge.source_id == SEEDFX_ENDPOINT_AUDIO
           && edge.destination_id == SEEDFX_ENDPOINT_AUDIO) continue;
        const int source_index = edge.source_id == SEEDFX_ENDPOINT_AUDIO
            ? -1 : FindNode(source, edge.source_id);
        const int destination_index = edge.destination_id == SEEDFX_ENDPOINT_AUDIO
            ? -1 : FindNode(source, edge.destination_id);
        if((edge.source_id != SEEDFX_ENDPOINT_AUDIO && source_index < 0)
           || (edge.destination_id != SEEDFX_ENDPOINT_AUDIO
               && destination_index < 0)
           || edge.source_id == edge.destination_id || !std::isfinite(edge.gain))
            return SeedFxGraphResult::InvalidEdge;
        if(destination_index >= 0 && source_index >= 0)
            ++indegree[destination_index];
    }

    uint8_t queue[SEEDFX_MAX_NODES]{};
    uint8_t head = 0U, tail = 0U;
    for(uint8_t i = 0; i < source.node_count; ++i)
        if(indegree[i] == 0U) queue[tail++] = i;
    uint8_t order_count = 0U;
    while(head != tail)
    {
        const uint8_t index = queue[head++];
        destination.order[order_count++] = index;
        for(uint8_t e = 0; e < source.edge_count; ++e)
        {
            const SeedFxEdgeDefinition& edge = source.edges[e];
            if(edge.source_id != source.nodes[index].id
               || edge.destination_id == SEEDFX_ENDPOINT_AUDIO) continue;
            const int next = FindNode(source, edge.destination_id);
            if(next >= 0 && --indegree[next] == 0U) queue[tail++] = next;
        }
    }
    if(order_count != source.node_count) return SeedFxGraphResult::Cycle;

    /* Compile reachability outside the audio ISR. A disabled physical output
     * can then remove its entire exclusive branch with a couple of bit ops. */
    for(unsigned ch=0; ch<kChannels; ++ch) {
        uint16_t needed=0;
        for(unsigned e=0; e<source.edge_count; ++e) {
            const auto& r=source.edges[e];
            if(!r.destination_id && r.destination_port==ch && r.source_id && r.gain!=0)
                needed |= 1U << FindNode(source,r.source_id);
        }
        for(unsigned order=order_count; order>0; --order) {
            const unsigned n=destination.order[order-1];
            if(!(needed & (1U<<n))) continue;
            for(unsigned e=0; e<source.edge_count; ++e) {
                const auto& r=source.edges[e];
                if(r.destination_id==source.nodes[n].id && r.source_id && r.gain!=0)
                    needed |= 1U << FindNode(source,r.source_id);
            }
        }
        destination.needed_by_output[ch]=needed;
    }

    destination.definition = source;
    destination.order_count = order_count;
    for(uint8_t i = 0; i < source.node_count; ++i)
    {
        destination.nodes[i] = {};
        destination.nodes[i].definition = source.nodes[i];
        for(uint8_t p = 0; p < SEEDFX_MAX_PARAMS; ++p)
            destination.nodes[i].parameter_cache[p] = source.nodes[i].parameters[p];
        seedfx_extension_defaults(source.nodes[i].effect_type,
            destination.nodes[i].parameter_cache, source.nodes[i].parameter_count);
        const uint16_t type = source.nodes[i].effect_type;
        if(type == SEEDFX_EFFECT_GAIN || type == SEEDFX_EFFECT_SOFT_CLIP
           || type == SEEDFX_EFFECT_HARD_CLIP || type == SEEDFX_EFFECT_COMPRESSOR
           || type == SEEDFX_EFFECT_LIMITER || type == SEEDFX_EFFECT_NOISE_GATE)
            destination.nodes[i].parameter_cache[0]
                = std::pow(10.0f, source.nodes[i].parameters[0] / 20.0f);
        if(type == SEEDFX_EFFECT_LIMITER)
            destination.nodes[i].parameter_cache[2] = std::pow(10.0f,
                destination.nodes[i].parameter_cache[2] / 20.0f);
        std::memcpy(destination.nodes[i].parameter_target,
                    destination.nodes[i].parameter_cache,
                    sizeof(destination.nodes[i].parameter_target));
    }
    return SeedFxGraphResult::Ok;
}

bool SameTopology(const RuntimeGraph& a, const RuntimeGraph& b)
{
    if(a.definition.node_count != b.definition.node_count
       || a.definition.edge_count != b.definition.edge_count) return false;
    for(uint8_t n = 0; n < a.definition.node_count; ++n)
        if(a.nodes[n].definition.id != b.nodes[n].definition.id
           || a.nodes[n].definition.effect_type != b.nodes[n].definition.effect_type
           || a.nodes[n].definition.package_id != b.nodes[n].definition.package_id)
            return false;
    return std::memcmp(a.definition.edges, b.definition.edges,
        a.definition.edge_count * sizeof(SeedFxEdgeDefinition)) == 0;
}

const float* SourceAudio(const RuntimeGraph& graph, uint16_t source_id,
                         uint8_t channel, const float* const input[2])
{
    if(source_id == SEEDFX_ENDPOINT_AUDIO) return input[channel];
    const int index = FindNode(graph.definition, source_id);
    return index >= 0 ? g_node_audio[index][channel] : nullptr;
}

void CopyStereo(const float source[2][kMaxFrames],
                float destination[2][kMaxFrames], std::size_t frames)
{
    for(uint8_t ch = 0; ch < 2U; ++ch)
        std::memcpy(destination[ch], source[ch], frames * sizeof(float));
}

void ProcessFilter(RuntimeNode& node, uint16_t type,
                   const float source[2][kMaxFrames],
                   float destination[2][kMaxFrames], std::size_t frames,
                   uint32_t sample_rate)
{
    const bool envelope_filter = type == SEEDFX_EFFECT_AUTO_WAH;
    const float fixed_alpha = envelope_filter ? 0.0f : Clamp(
        2.0f * std::sin(kPi * Clamp(node.parameter_cache[0], 20.0f,
            sample_rate * .42f) / sample_rate), .001f, .95f);
    const float fixed_damping = 1.9f
        - Clamp(node.parameter_cache[1], 0, .95f) * 1.75f;
    for(std::size_t i = 0; i < frames; ++i)
    {
        float cutoff = node.parameter_cache[0];
        float resonance = node.parameter_cache[1];
        if(type == SEEDFX_EFFECT_AUTO_WAH)
        {
            const float envelope = .5f * (std::fabs(source[0][i])
                                           + std::fabs(source[1][i]));
            node.state[0][7] += .01f * (envelope - node.state[0][7]);
            cutoff = node.parameter_cache[1] + node.parameter_cache[2]
                     * Clamp(node.state[0][7]
                             * (1.0f + node.parameter_cache[0] * 8.0f), 0, 1);
            resonance = node.parameter_cache[3];
        }
        const float alpha = envelope_filter ? Clamp(2.0f * std::sin(
            kPi * Clamp(cutoff, 20.0f, sample_rate * .42f) / sample_rate),
            .001f, .95f) : fixed_alpha;
        const float damping = envelope_filter
            ? 1.9f - Clamp(resonance, 0, .95f) * 1.75f : fixed_damping;
        for(uint8_t ch = 0; ch < 2U; ++ch)
        {
            float& low = node.state[ch][0];
            float& band = node.state[ch][1];
            const float high = source[ch][i] - low - damping * band;
            band += alpha * high;
            low += alpha * band;
            if(type == SEEDFX_EFFECT_LOWPASS) destination[ch][i] = low;
            else if(type == SEEDFX_EFFECT_HIGHPASS) destination[ch][i] = high;
            else if(type == SEEDFX_EFFECT_NOTCH) destination[ch][i] = low + high;
            else destination[ch][i] = band;
        }
    }
}

void ProcessNode(RuntimeNode& node, uint32_t graph_slot, uint8_t node_index,
                 const float source[2][kMaxFrames],
                 float destination[2][kMaxFrames], std::size_t frames,
                 uint32_t sample_rate)
{
    const uint16_t type = node.definition.effect_type;
    if((node.definition.flags & SEEDFX_NODE_ENABLED) == 0U
       || type == SEEDFX_EFFECT_BYPASS)
    { CopyStereo(source, destination, frames); return; }
    if(type == SEEDFX_EFFECT_MUTE)
    {
        for(uint8_t ch = 0; ch < 2U; ++ch)
            std::memset(destination[ch], 0, sizeof(float) * frames);
        return;
    }
    if(type >= SEEDFX_EFFECT_LOWPASS && type <= SEEDFX_EFFECT_NOTCH)
    { ProcessFilter(node, type, source, destination, frames, sample_rate); return; }
    if(type == SEEDFX_EFFECT_AUTO_WAH)
    { ProcessFilter(node, type, source, destination, frames, sample_rate); return; }
    if(type == SEEDFX_EFFECT_CHANNEL_SWAP)
    {
        for(std::size_t i = 0; i < frames; ++i)
        { destination[0][i] = source[1][i]; destination[1][i] = source[0][i]; }
        return;
    }
    if(type == SEEDFX_EFFECT_MONO)
    {
        for(std::size_t i = 0; i < frames; ++i)
            destination[0][i] = destination[1][i]
                = .5f * (source[0][i] + source[1][i]);
        return;
    }

    const float p0 = node.parameter_cache[0], p1 = node.parameter_cache[1];
    const float p2 = node.parameter_cache[2], p3 = node.parameter_cache[3];
    const float inv_rate = 1.0f / sample_rate;
    const bool delay_family = (type >= SEEDFX_EFFECT_DELAY
                               && type <= SEEDFX_EFFECT_VIBRATO)
                              || type == SEEDFX_EFFECT_REVERB
                              || type == SEEDFX_EFFECT_DOUBLER;
    float* delay[2] = {nullptr, nullptr};
    if(delay_family)
        for(uint8_t ch = 0; ch < 2U; ++ch)
            delay[ch] = DelayBuffer(graph_slot, node_index, ch);
    const float tone_boost = (type == SEEDFX_EFFECT_BASS_BOOST
                              || type == SEEDFX_EFFECT_TREBLE_BOOST)
        ? std::pow(10.0f, Clamp(p1, 0, 18) / 20.0f) - 1.0f : 0.0f;
    const float dynamics_attack = (type == SEEDFX_EFFECT_COMPRESSOR
                                   || type == SEEDFX_EFFECT_NOISE_GATE)
        ? Clamp(1.0f / (p2 * .001f * sample_rate), .0001f, 1.0f) : 1.0f;
    const float dynamics_release = (type >= SEEDFX_EFFECT_COMPRESSOR
                                    && type <= SEEDFX_EFFECT_NOISE_GATE)
        ? Clamp(1.0f / (((type == SEEDFX_EFFECT_COMPRESSOR ? p3 : p1)
                         * .001f * sample_rate)), .00001f, 1.0f) : 1.0f;

    for(std::size_t i = 0; i < frames; ++i)
    {
        float left = source[0][i], right = source[1][i];
        switch(type)
        {
            case SEEDFX_EFFECT_SPLITTER:
                right = left;
                break;
            case SEEDFX_EFFECT_MIXER:
                left = (left * Clamp(p0,0,1) + right * Clamp(p1,0,1)) * Clamp(p2,0,1);
                right = 0;
                break;
            case SEEDFX_EFFECT_GAIN:
                left *= p0 * (1 - fmaxf(0, p1));
                right *= p0 * (1 + fminf(0, p1)); break;
            case SEEDFX_EFFECT_POLARITY: left = -left; right = -right; break;
            case SEEDFX_EFFECT_SOFT_CLIP:
            {
                const float mix = Clamp(p1, 0, 1);
                left += (SoftClip(left * p0) - left) * mix;
                right += (SoftClip(right * p0) - right) * mix;
                left *= p2; right *= p2;
                break;
            }
            case SEEDFX_EFFECT_HARD_CLIP:
            {
                const float level = Clamp(p1, .05f, 1.0f);
                left += (Clamp(left * p0, -level, level) - left) * Clamp(p2, 0, 1);
                right += (Clamp(right * p0, -level, level) - right) * Clamp(p2, 0, 1);
                break;
            }
            case SEEDFX_EFFECT_OVERDRIVE:
            {
                const float drive = 1.0f + Clamp(p0, 0, 1) * 18.0f;
                const float alpha = .02f + Clamp(p1, 0, 1) * .45f;
                for(uint8_t ch = 0; ch < 2U; ++ch)
                {
                    const float x = SoftClip(source[ch][i] * drive);
                    node.state[ch][0] += alpha * (x - node.state[ch][0]);
                    destination[ch][i] = node.state[ch][0] * Clamp(p2, 0, 1.5f);
                }
                continue;
            }
            case SEEDFX_EFFECT_FUZZ:
            {
                const float gain = 2.0f + Clamp(p0, 0, 1) * 28.0f;
                left = Clamp((left + p1) * gain, -1, 1) * p2;
                right = Clamp((right + p1) * gain, -1, 1) * p2;
                break;
            }
            case SEEDFX_EFFECT_WAVEFOLDER:
            {
                const float mix = Clamp(p1, 0, 1);
                for(uint8_t ch = 0; ch < 2U; ++ch)
                {
                    float folded = source[ch][i] * Clamp(p0, 1, 8);
                    for(uint8_t fold = 0; fold < 4U; ++fold)
                    {
                        if(folded > 1) folded = 2 - folded;
                        else if(folded < -1) folded = -2 - folded;
                    }
                    destination[ch][i] = source[ch][i]
                        + (Clamp(folded, -1, 1) - source[ch][i]) * mix;
                    destination[ch][i] *= p2;
                }
                continue;
            }
            case SEEDFX_EFFECT_BITCRUSHER:
            {
                const uint32_t bits = static_cast<uint32_t>(std::round(Clamp(p0, 4, 24)));
                const float levels = static_cast<float>(1U << bits);
                const float mix = Clamp(p1, 0, 1);
                left += (std::floor(left * levels) / levels - left) * mix;
                right += (std::floor(right * levels) / levels - right) * mix;
                break;
            }
            case SEEDFX_EFFECT_SAMPLE_RATE_REDUCER:
            {
                const uint32_t divide = static_cast<uint32_t>(std::round(Clamp(p0, 1, 32)));
                if(node.hold_counter++ % divide == 0U)
                { node.hold[0] = left; node.hold[1] = right; }
                const float mix = Clamp(p1, 0, 1);
                left += (node.hold[0] - left) * mix;
                right += (node.hold[1] - right) * mix;
                break;
            }
            case SEEDFX_EFFECT_BASS_BOOST:
            case SEEDFX_EFFECT_TREBLE_BOOST:
            {
                const float alpha = Clamp(2 * kPi * p0 * inv_rate, .001f, .8f);
                for(uint8_t ch = 0; ch < 2U; ++ch)
                {
                    node.state[ch][0] += alpha * (source[ch][i] - node.state[ch][0]);
                    const float band = type == SEEDFX_EFFECT_BASS_BOOST
                        ? node.state[ch][0] : source[ch][i] - node.state[ch][0];
                    destination[ch][i] = source[ch][i] + band * tone_boost;
                }
                continue;
            }
            case SEEDFX_EFFECT_COMPRESSOR:
            case SEEDFX_EFFECT_LIMITER:
            case SEEDFX_EFFECT_NOISE_GATE:
            {
                const float threshold = Clamp(p0, .0001f, 1);
                for(uint8_t ch = 0; ch < 2U; ++ch)
                {
                    const float x = source[ch][i] * (type == SEEDFX_EFFECT_LIMITER ? p2 : 1.0f);
                    const float magnitude = std::fabs(x);
                    float& env = node.state[ch][0];
                    env += (magnitude - env) * (magnitude > env
                        ? dynamics_attack : dynamics_release);
                    float gain = 1.0f;
                    if(type == SEEDFX_EFFECT_NOISE_GATE)
                        gain = env >= threshold ? 1.0f : Clamp(env / threshold, 0, 1);
                    else if(env > threshold)
                    {
                        const float ratio = type == SEEDFX_EFFECT_LIMITER ? 100.0f : Clamp(p1, 1, 20);
                        gain = (threshold + (env - threshold) / ratio) / env;
                    }
                    destination[ch][i] = x * gain;
                }
                continue;
            }
            case SEEDFX_EFFECT_TREMOLO:
            case SEEDFX_EFFECT_AUTO_PAN:
            case SEEDFX_EFFECT_RING_MOD:
            case SEEDFX_EFFECT_ROTARY:
            {
                const float lfo = Triangle(node.phase);
                AdvancePhase(node, p0, sample_rate);
                if(type == SEEDFX_EFFECT_TREMOLO)
                {
                    const float gain = 1 - Clamp(p1, 0, 1) * (.5f + .5f * lfo);
                    left *= gain; right *= gain;
                }
                else if(type == SEEDFX_EFFECT_AUTO_PAN)
                {
                    const float depth = Clamp(p1, 0, 1);
                    left *= 1 - depth * (.5f + .5f * lfo);
                    right *= 1 - depth * (.5f - .5f * lfo);
                }
                else if(type == SEEDFX_EFFECT_RING_MOD)
                {
                    const float mix = Clamp(p1, 0, 1);
                    left += (left * lfo - left) * mix;
                    right += (right * lfo - right) * mix;
                }
                else
                {
                    const float depth = Clamp(p1, 0, 1), mix = Clamp(p2, 0, 1);
                    const float wet_l = left * (1 - depth * (.5f + .5f * lfo));
                    const float wet_r = right * (1 - depth * (.5f - .5f * lfo));
                    left += (wet_l - left) * mix; right += (wet_r - right) * mix;
                }
                break;
            }
            case SEEDFX_EFFECT_DELAY:
            case SEEDFX_EFFECT_PING_PONG_DELAY:
            case SEEDFX_EFFECT_SLAPBACK:
            {
                const uint32_t d = static_cast<uint32_t>(Clamp(
                    p0 * .001f * sample_rate, 1, kDelayFrames - 1));
                const float alpha = .02f + .98f * Clamp(p3, 0, 1);
                node.state[0][0] += alpha * (DelayRead(node, delay[0], d) - node.state[0][0]);
                node.state[1][0] += alpha * (DelayRead(node, delay[1], d) - node.state[1][0]);
                const float wet_l = node.state[0][0], wet_r = node.state[1][0];
                const float feedback = Clamp(p1, 0, .95f), mix = Clamp(p2, 0, 1);
                delay[0][node.write_index] = left
                    + (type == SEEDFX_EFFECT_PING_PONG_DELAY ? wet_r : wet_l) * feedback;
                delay[1][node.write_index] = right
                    + (type == SEEDFX_EFFECT_PING_PONG_DELAY ? wet_l : wet_r) * feedback;
                left += (wet_l - left) * mix; right += (wet_r - right) * mix;
                break;
            }
            case SEEDFX_EFFECT_CHORUS:
            case SEEDFX_EFFECT_FLANGER:
            case SEEDFX_EFFECT_VIBRATO:
            case SEEDFX_EFFECT_DOUBLER:
            {
                const float rate = type == SEEDFX_EFFECT_DOUBLER ? .7f : p0;
                const float lfo = Triangle(node.phase);
                AdvancePhase(node, rate, sample_rate);
                const float base_ms = type == SEEDFX_EFFECT_FLANGER ? 2.0f
                    : (type == SEEDFX_EFFECT_DOUBLER ? p0 : 18.0f);
                const float depth_ms = type == SEEDFX_EFFECT_DOUBLER ? p1 * 3 : p1;
                const uint32_t dl = static_cast<uint32_t>(Clamp(
                    (base_ms + depth_ms * (.5f + .5f * lfo)) * .001f * sample_rate,
                    1, kDelayFrames - 1));
                const float stereo_lfo = type == SEEDFX_EFFECT_CHORUS
                    ? lfo * (1 - 2 * Clamp(p3, 0, 1)) : -lfo;
                const uint32_t dr = static_cast<uint32_t>(Clamp(
                    (base_ms + depth_ms * (.5f + .5f * stereo_lfo)) * .001f * sample_rate,
                    1, kDelayFrames - 1));
                const float wet_l = DelayRead(node, delay[0], dl);
                const float wet_r = DelayRead(node, delay[1], dr);
                const float feedback = type == SEEDFX_EFFECT_FLANGER
                    ? Clamp(p2, -.9f, .9f) : 0.0f;
                delay[0][node.write_index] = left + wet_l * feedback;
                delay[1][node.write_index] = right + wet_r * feedback;
                const float mix = type == SEEDFX_EFFECT_VIBRATO ? Clamp(p2, 0, 1)
                    : (type == SEEDFX_EFFECT_FLANGER ? Clamp(p3, 0, 1)
                       : Clamp(p2, 0, 1));
                left += (wet_l - left) * mix; right += (wet_r - right) * mix;
                break;
            }
            case SEEDFX_EFFECT_PHASER:
            {
                const float lfo = Triangle(node.phase);
                AdvancePhase(node, p0, sample_rate);
                const float a = Clamp(.05f + (.45f + .4f * lfo) * p1, .02f, .92f);
                for(uint8_t ch = 0; ch < 2U; ++ch)
                {
                    float y = source[ch][i] + node.state[ch][4] * Clamp(p2, -.9f, .9f);
                    for(uint8_t stage = 0; stage < 4U; ++stage)
                    {
                        const float next = -a * y + node.state[ch][stage];
                        node.state[ch][stage] = y + a * next; y = next;
                    }
                    node.state[ch][4] = y;
                    destination[ch][i] = source[ch][i]
                        + (y - source[ch][i]) * Clamp(p3, 0, 1);
                }
                continue;
            }
            case SEEDFX_EFFECT_STEREO_WIDENER:
            {
                const float mid = .5f * (left + right);
                const float side = .5f * (left - right) * Clamp(p0, 0, 2);
                left = (mid + side) * (1 - fmaxf(0, p1));
                right = (mid - side) * (1 + fminf(0, p1)); break;
            }
            case SEEDFX_EFFECT_DC_BLOCKER:
            {
                const float pole = Clamp(1 - 2 * kPi * p0 * inv_rate, .9f, .99999f);
                for(uint8_t ch = 0; ch < 2U; ++ch)
                {
                    const float x = source[ch][i];
                    const float y = x - node.state[ch][0] + pole * node.state[ch][1];
                    node.state[ch][0] = x; node.state[ch][1] = y;
                    destination[ch][i] = y;
                }
                continue;
            }
            case SEEDFX_EFFECT_REVERB:
            {
                const uint32_t dl = static_cast<uint32_t>(sample_rate * (.071f + .11f * p0));
                const uint32_t dr = static_cast<uint32_t>(sample_rate * (.093f + .13f * p0));
                const float wet_l = DelayRead(node, delay[0], dl);
                const float wet_r = DelayRead(node, delay[1], dr);
                const float alpha = .02f + .4f * (1 - Clamp(p1, 0, 1));
                node.state[0][0] += alpha * (wet_r - node.state[0][0]);
                node.state[1][0] += alpha * (wet_l - node.state[1][0]);
                const float feedback = Clamp(p3, 0, .92f);
                delay[0][node.write_index] = left + node.state[0][0] * feedback;
                delay[1][node.write_index] = right + node.state[1][0] * feedback;
                const float mix = Clamp(p2, 0, 1);
                left += (wet_l - left) * mix; right += (wet_r - right) * mix;
                break;
            }
            case SEEDFX_EFFECT_CABINET_SIM:
            {
                for(uint8_t ch = 0; ch < 2U; ++ch)
                {
                    const float x = SoftClip(source[ch][i] * (1.5f + p0 * 2));
                    node.state[ch][0] += .16f * (x - node.state[ch][0]);
                    node.state[ch][1] += .03f * (node.state[ch][0] - node.state[ch][1]);
                    destination[ch][i] = node.state[ch][1] * (1.15f - .35f * p0)
                        + (node.state[ch][0] - node.state[ch][1]) * p1;
                    destination[ch][i] = source[ch][i]
                        + (destination[ch][i] - source[ch][i]) * Clamp(p2, 0, 1);
                }
                continue;
            }
            case SEEDFX_EFFECT_EXCITER:
            {
                const float alpha = Clamp(2 * kPi * p1 * inv_rate, .01f, .9f);
                for(uint8_t ch = 0; ch < 2U; ++ch)
                {
                    node.state[ch][0] += alpha * (source[ch][i] - node.state[ch][0]);
                    const float high = source[ch][i] - node.state[ch][0];
                    destination[ch][i] = source[ch][i]
                        + SoftClip(high * (1 + p0 * 12)) * Clamp(p2, 0, 1);
                }
                continue;
            }
            default: break;
        }
        destination[0][i] = left;
        destination[1][i] = right;
        if(delay_family)
        {
            if(++node.write_index == kDelayFrames) node.write_index = 0U;
            if(node.valid_frames < kDelayFrames) ++node.valid_frames;
        }
    }
}
} // namespace

void seedfx_graph_init()
{
    Arena().magic = kArenaMagic;
    Arena().bytes_used = sizeof(ArenaHeader);
    Arena().package_count = 0U;
    g_upload = {};
    g_active_graph = 0U;
    g_pending_graph = UINT32_MAX;
    g_graph_commits = g_graph_rejects = 0U;

    SeedFxGraphDefinition graph{};
    graph.magic = SEEDFX_GRAPH_MAGIC;
    graph.version = SEEDFX_GRAPH_VERSION;
    graph.bytes = sizeof(graph);
    graph.revision = 1U;
    graph.flags = SEEDFX_GRAPH_ENABLED;
    graph.edge_count = 2U;
    std::memcpy(graph.name, "Default thru", 12U);
    graph.edges[0] = {SEEDFX_ENDPOINT_AUDIO, SEEDFX_ENDPOINT_AUDIO, 1.0f};
    graph.edges[1] = {SEEDFX_ENDPOINT_AUDIO, SEEDFX_ENDPOINT_AUDIO, 1.0f, 1, 1, 0};
    graph.crc32 = seedfx_graph_crc32(&graph);
    (void)BuildRuntime(graph, g_graphs[0]);
    g_graphs[1] = g_graphs[0];
}

SeedFxGraphResult seedfx_graph_stage(const SeedFxGraphDefinition& graph)
{
    if(__atomic_load_n(&g_pending_graph, __ATOMIC_ACQUIRE) != UINT32_MAX)
        return SeedFxGraphResult::GraphBusy;
    const uint32_t staging = 1U - __atomic_load_n(&g_active_graph, __ATOMIC_ACQUIRE);
    const SeedFxGraphResult result = BuildRuntime(graph, g_graphs[staging]);
    if(result == SeedFxGraphResult::Ok)
        __atomic_store_n(&g_pending_graph, staging, __ATOMIC_RELEASE);
    else ++g_graph_rejects;
    return result;
}

void seedfx_graph_process(const float* const input[2], float* const output[2],
                          std::size_t frames, uint32_t sample_rate, bool enabled,
                          uint8_t output_mask)
{
    if(frames > kMaxFrames) frames = kMaxFrames;
    if(sample_rate < 8000U || sample_rate > 192000U) sample_rate = 48000U;
    const uint32_t pending = __atomic_exchange_n(&g_pending_graph, UINT32_MAX,
                                                  __ATOMIC_ACQ_REL);
    if(pending < 2U)
    {
        RuntimeGraph& active = g_graphs[g_active_graph];
        const RuntimeGraph& update = g_graphs[pending];
        if(SameTopology(active, update))
        {
            /* Keep delay bank, phase, filters and envelope followers alive
             * while turning knobs. Only the audio callback owns DSP state. */
            active.definition = update.definition;
            for(uint8_t n = 0; n < active.definition.node_count; ++n)
            {
                active.nodes[n].definition = update.nodes[n].definition;
                std::memcpy(active.nodes[n].parameter_target,
                            update.nodes[n].parameter_target,
                            sizeof(active.nodes[n].parameter_target));
            }
        }
        else __atomic_store_n(&g_active_graph, pending, __ATOMIC_RELEASE);
        ++g_graph_commits;
    }
    const uint32_t graph_slot = __atomic_load_n(&g_active_graph, __ATOMIC_ACQUIRE);
    RuntimeGraph& graph = g_graphs[graph_slot];
    if(!enabled || (graph.definition.flags & SEEDFX_GRAPH_ENABLED) == 0U)
    {
        for(uint8_t ch = 0; ch < 2U; ++ch)
            std::memcpy(output[ch], input[ch], frames * sizeof(float));
        return;
    }
    const uint16_t needed = ((output_mask&1) ? graph.needed_by_output[0] : 0)
                          | ((output_mask&2) ? graph.needed_by_output[1] : 0);

    for(uint8_t order = 0; order < graph.order_count; ++order)
    {
        const uint8_t node_index = graph.order[order];
        if(!(needed & (1U<<node_index))) continue;
        /* About 5 ms settling time; no expensive conversion in the ISR. */
        RuntimeNode& node = graph.nodes[node_index];
        const float slew = Clamp(static_cast<float>(frames) / (.005f * sample_rate), 0, 1);
        for(uint8_t p = 0; p < node.definition.parameter_count; ++p)
            node.parameter_cache[p] += slew
                * (node.parameter_target[p] - node.parameter_cache[p]);
        float mixed[2][kMaxFrames]{};
        const unsigned inputs = seedfx_input_channels(node.definition.effect_type);
        for(uint8_t e = 0; e < graph.definition.edge_count; ++e)
        {
            const SeedFxEdgeDefinition& edge = graph.definition.edges[e];
            if(edge.destination_id != graph.definition.nodes[node_index].id) continue;
            const float* source = SourceAudio(graph, edge.source_id, edge.source_port, input);
            if(source == nullptr) continue;
            for(std::size_t frame = 0; frame < frames; ++frame)
                mixed[edge.destination_port][frame] += source[frame] * edge.gain;
        }
        /* Mono-input stereo processors operate on one signal and generate
         * stereo internally. The second physical input is never summed in. */
        if(inputs == 1) std::memcpy(mixed[1], mixed[0], frames * sizeof(float));
        ProcessNode(graph.nodes[node_index], graph_slot, node_index, mixed,
                    g_node_audio[node_index], frames, sample_rate);
        if(seedfx_output_channels(node.definition.effect_type) == 1)
            std::memset(g_node_audio[node_index][1], 0, frames * sizeof(float));
    }

    for(uint8_t ch = 0; ch < 2U; ++ch)
        std::memset(output[ch], 0, frames * sizeof(float));
    for(uint8_t e = 0; e < graph.definition.edge_count; ++e)
    {
        const SeedFxEdgeDefinition& edge = graph.definition.edges[e];
        if(edge.destination_id != SEEDFX_ENDPOINT_AUDIO) continue;
        if(!(output_mask&(1U<<edge.destination_port))) continue;
        const float* source = SourceAudio(graph, edge.source_id, edge.source_port, input);
        if(source == nullptr) continue;
        for(std::size_t frame = 0; frame < frames; ++frame)
            output[edge.destination_port][frame] += source[frame] * edge.gain;
    }
}

SeedFxGraphResult seedfx_cache_begin(uint32_t package_id, uint32_t total_bytes,
                                     uint32_t content_crc32)
{
    if(g_upload.active) return SeedFxGraphResult::CacheBusy;
    const uint32_t aligned = (Arena().bytes_used + 31U) & ~31U;
    if(total_bytes == 0U || aligned > kCacheLimit
       || total_bytes > kCacheLimit - aligned) return SeedFxGraphResult::CacheBounds;
    g_upload = {true, package_id, aligned, total_bytes, 0U,
                content_crc32, 0xffffffffU};
    return SeedFxGraphResult::Ok;
}

SeedFxGraphResult seedfx_cache_write(uint32_t package_id, uint32_t offset,
                                     const uint8_t* data, uint32_t bytes)
{
    if(!g_upload.active || package_id != g_upload.package_id
       || offset != g_upload.written) return SeedFxGraphResult::CacheSequence;
    if(data == nullptr || bytes > g_upload.total_bytes - g_upload.written)
        return SeedFxGraphResult::CacheBounds;
    std::memcpy(g_arena + g_upload.start + offset, data, bytes);
    g_upload.running_crc = seedfx_crc32_update(g_upload.running_crc, data, bytes);
    g_upload.written += bytes;
    return SeedFxGraphResult::Ok;
}

SeedFxGraphResult seedfx_cache_end(uint32_t package_id)
{
    if(!g_upload.active || package_id != g_upload.package_id
       || g_upload.written != g_upload.total_bytes) return SeedFxGraphResult::CacheSequence;
    if(~g_upload.running_crc != g_upload.expected_crc)
    { g_upload = {}; return SeedFxGraphResult::CacheCrc; }
    Arena().bytes_used = g_upload.start + g_upload.total_bytes;
    ++Arena().package_count;
    g_upload = {};
    return SeedFxGraphResult::Ok;
}

void seedfx_cache_clear()
{
    Arena().bytes_used = sizeof(ArenaHeader);
    Arena().package_count = 0U;
    g_upload = {};
}

void seedfx_graph_get_stats(SeedFxGraphStats& stats)
{
    const uint32_t active = __atomic_load_n(&g_active_graph, __ATOMIC_ACQUIRE);
    stats.active_revision = g_graphs[active].definition.revision;
    stats.active_nodes = g_graphs[active].definition.node_count;
    stats.graph_commits = g_graph_commits;
    stats.graph_rejects = g_graph_rejects;
    stats.cache_bytes_used = Arena().bytes_used;
    stats.cache_capacity = kCacheLimit;
    stats.cached_packages = Arena().package_count;
}

const char* seedfx_graph_result_name(SeedFxGraphResult result)
{
    switch(result)
    {
        case SeedFxGraphResult::Ok: return "OK";
        case SeedFxGraphResult::InvalidHeader: return "HEADER";
        case SeedFxGraphResult::InvalidCrc: return "CRC";
        case SeedFxGraphResult::InvalidNode: return "NODE";
        case SeedFxGraphResult::InvalidEdge: return "EDGE";
        case SeedFxGraphResult::Cycle: return "CYCLE";
        case SeedFxGraphResult::GraphBusy: return "GRAPH_BUSY";
        case SeedFxGraphResult::CacheBusy: return "CACHE_BUSY";
        case SeedFxGraphResult::CacheBounds: return "CACHE_BOUNDS";
        case SeedFxGraphResult::CacheSequence: return "CACHE_SEQUENCE";
        case SeedFxGraphResult::CacheCrc: return "CACHE_CRC";
    }
    return "UNKNOWN";
}
