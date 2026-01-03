# VP9 Codec Support Status

## Current Status: **WORKING via DataChannel + WebCodecs** ✅

VP9 is fully implemented using DataChannel delivery with WebCodecs API decoding!

## How It Works 🔧

**VP9 via DataChannel + WebCodecs:**

1. **Server** - Encodes frames with libvpx, sends via DataChannel
2. **Client** - Receives via DataChannel, decodes with WebCodecs VideoDecoder API
3. **Rendering** - Draws to canvas (similar to PNG mode)

This bypasses the need for VP9RtpPacketizer in libdatachannel!

## Browser Requirements

**WebCodecs API support:**
- ✅ Chrome 94+ (Sept 2021)
- ✅ Edge 94+ (Sept 2021)  
- ✅ Firefox 130+ (Sept 2024)
- ❌ Safari (not yet)

## Usage

Select VP9 from codec dropdown or edit prefs:
```
webcodec vp9
```

## Why VP9 is Great

- ✅ Screen content coding (sharp UI/text)
- ✅ ~30% better than H.264 for UI
- ✅ Faster encoding than AV1
- ✅ Works today!

## Future: VP9 via RTP

When libdatachannel adds VP9RtpPacketizer, we can switch to lower-latency UDP delivery.
