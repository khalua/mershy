# AI Buddy

A Tamagotchi-style buddy for the Waveshare ESP32-S3-Touch-AMOLED-1.75C. Tap
the screen and talk; it answers with Claude, out loud and on screen.

Needs `ANTHROPIC_API_KEY`, `DEEPGRAM_API_KEY` and `ELEVENLABS_API_KEY` in
your shell env (or a project-root `.env`) at build time.

```bash
cp src/secrets.example.h src/secrets.h
pio run -e ai-buddy -t upload && pio device monitor -b 115200
```

See [CLAUDE.md](CLAUDE.md) for architecture and toolchain notes.
