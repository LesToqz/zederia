// The engine's own event type, so the core needs no plug-in framework.
#pragma once

#include <cstdint>

namespace zed {

struct MidiEvent {
    enum class Type : std::uint8_t {
        NoteOn,          // number = key, value = velocity 0..1
        NoteOff,         // number = key, value = release velocity 0..1
        PitchBend,       // value = -1..1
        Controller,      // number = controller, value = 0..1
        ChannelPressure, // value = 0..1
        PolyPressure,    // number = key, value = 0..1
    };

    int offset = 0; // sample position inside the block
    Type type = Type::NoteOn;
    std::uint8_t channel = 0;
    std::uint8_t number = 0;
    float value = 0.0f;

    static MidiEvent noteOn(int offset, int key, float velocity, int channel = 0) {
        return {offset, Type::NoteOn, static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(key), velocity};
    }
    static MidiEvent noteOff(int offset, int key, float velocity = 0.0f, int channel = 0) {
        return {offset, Type::NoteOff, static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(key), velocity};
    }
    static MidiEvent pitchBend(int offset, float value, int channel = 0) {
        return {offset, Type::PitchBend, static_cast<std::uint8_t>(channel), 0, value};
    }
    static MidiEvent controller(int offset, int number, float value, int channel = 0) {
        return {offset, Type::Controller, static_cast<std::uint8_t>(channel), static_cast<std::uint8_t>(number), value};
    }
};

// Converts a three-byte channel message. Returns false for messages the engine does not use.
// A note-on with velocity zero becomes a note-off.
inline bool midiFromBytes(std::uint8_t status, std::uint8_t data1, std::uint8_t data2, int offset, MidiEvent& out) {
    data1 &= 0x7f;
    data2 &= 0x7f;
    out.offset = offset;
    out.channel = static_cast<std::uint8_t>(status & 0x0f);
    out.number = data1;
    switch (status & 0xf0) {
    case 0x90:
        out.type = data2 == 0 ? MidiEvent::Type::NoteOff : MidiEvent::Type::NoteOn;
        out.value = static_cast<float>(data2) / 127.0f;
        return true;
    case 0x80:
        out.type = MidiEvent::Type::NoteOff;
        out.value = static_cast<float>(data2) / 127.0f;
        return true;
    case 0xA0:
        out.type = MidiEvent::Type::PolyPressure;
        out.value = static_cast<float>(data2) / 127.0f;
        return true;
    case 0xB0:
        out.type = MidiEvent::Type::Controller;
        out.value = static_cast<float>(data2) / 127.0f;
        return true;
    case 0xD0:
        out.type = MidiEvent::Type::ChannelPressure;
        out.number = 0;
        out.value = static_cast<float>(data1) / 127.0f;
        return true;
    case 0xE0: {
        const int raw = (static_cast<int>(data2) << 7) | data1; // 0..16383, centre 8192
        out.type = MidiEvent::Type::PitchBend;
        out.number = 0;
        out.value = raw >= 8192 ? static_cast<float>(raw - 8192) / 8191.0f : static_cast<float>(raw - 8192) / 8192.0f;
        return true;
    }
    default:
        return false;
    }
}

} // namespace zed
