# Mershy art guide

How to generate the cartoon Mehrshad artwork for the round AMOLED.

## How the art is used

The firmware draws Mershy in **layers**:

- **Base:** the whole portrait (hair, face, beard, earrings, tank top) drawn once, never redrawn.
- **Eyes, brows, mouth:** small rectangles cut from variant images and swapped on top of the base to blink, emote and lip-sync.

So you generate **one master portrait**, then **edited copies of it where only one feature changes**. A script (written once your master exists) lines each copy up with the master, cuts out the feature region with soft edges, and converts it for the firmware. That's why every copy must keep the exact same framing, pose and lighting.

## Workflow (ChatGPT image generation)

1. Start a new ChatGPT chat. Upload the photo of Mehrshad and paste the **master prompt** below.
2. Regenerate until you love it. This is the one image that matters most. Download it as `master.png`.
3. In the **same chat**, for each variant below, paste the **variant prefix** plus that variant's line. Download each result with the file name in the table.
4. Compare each variant with the master (flip between them): only the named feature should change. If the head moved, the hair changed or the colors shifted, regenerate it. Small drift is fine; the script feathers the edges.
5. Put all PNGs in `art/source/`. Any square size works (1024×1024 is typical).

## Master prompt

> Create a cartoon character portrait based on the man in this photo. Keep his likeness: long, voluminous, curly dark-brown hair with gray streaks, full short beard with gray, thick dark eyebrows, warm brown eyes, big friendly toothy smile, small silver hoop earrings, light blue-gray ribbed tank top.
>
> Style: clean modern 2D cartoon illustration, smooth bold outlines, flat cel shading with soft highlights, warm vibrant colors, friendly and expressive, like a high-quality animated TV character. Slightly larger head and eyes than real proportions, but still clearly him.
>
> Composition: square 1:1 image. Head and top of the shoulders only, facing straight toward the viewer, looking directly at the camera, head level and centered. The head and hair fill about 75% of the frame's height, with everything important inside a centered circle that touches the edges of the square (the image will be shown on a round screen). Mouth closed in a gentle, relaxed smile. Eyes fully open.
>
> Background: pure solid black (#000000), no gradient, no texture, no props, no text. Add a soft warm rim light along the outer edge of his hair and shoulders so the dark hair stands out clearly against the black.

Why these rules:
- **Pure black background:** black pixels are switched off on AMOLED, so he floats on the glass.
- **Rim light:** dark hair on black disappears without it.
- **Mouth closed, eyes open:** the neutral starting point every variant edits from.

## Variant prefix

Paste this before each variant line:

> Edit the master image. Keep EVERYTHING identical — same framing, head position and size, hair, beard, clothing, colors, line style, lighting and black background. Change ONLY the following:

## Variants

### Mouth (lip-sync and moods)

| File | Variant line |
|---|---|
| `mouth_grin.png` | His mouth in a big, wide, open toothy grin, like his real smile — upper teeth showing, very happy. |
| `mouth_talk_small.png` | His mouth slightly open mid-word, as if saying "eh" — a small gap between the lips, teeth barely visible. |
| `mouth_talk_mid.png` | His mouth open medium-wide mid-word, as if saying "ah" — upper teeth and some tongue visible. |
| `mouth_talk_wide.png` | His mouth open wide mid-word, as if saying a loud "AH" — clearly open, teeth and tongue visible. |
| `mouth_oo.png` | His lips rounded into a small "oo" shape, as if saying "ooh" with interest. |
| `mouth_flat.png` | His mouth closed in a flat, neutral straight line — no smile. |
| `mouth_frown.png` | His mouth closed in a gentle sad frown, corners turned down. |

### Eyes

| File | Variant line |
|---|---|
| `eyes_closed.png` | Both eyes fully closed, relaxed (mid-blink). Eyebrows unchanged. |
| `eyes_half.png` | Both eyes half-closed, sleepy. Eyebrows unchanged. |
| `eyes_happy.png` | Both eyes squinted into happy upward curves, as when laughing. Eyebrows unchanged. |
| `eyes_wide.png` | Both eyes wide open and bright, attentive, pupils slightly larger. Eyebrows unchanged. |
| `eyes_left.png` | Both pupils looking to HIS right (the viewer's left). Head does not turn. Eyebrows unchanged. |
| `eyes_right.png` | Both pupils looking to HIS left (the viewer's right). Head does not turn. Eyebrows unchanged. |
| `eyes_up.png` | Both pupils looking up and to the side, as if thinking. Head does not turn. Eyebrows unchanged. |

### Eyebrows

| File | Variant line |
|---|---|
| `brows_raised.png` | Both eyebrows raised high, curious and surprised. Eyes unchanged. |
| `brows_sad.png` | Eyebrows tilted into a sad, worried shape (inner ends raised). Eyes unchanged. |
| `brows_furrowed.png` | Eyebrows lowered and pulled together, concentrating or puzzled. Eyes unchanged. |
| `brows_one_up.png` | Only his left eyebrow (viewer's right) raised, skeptical and curious. Eyes unchanged. |

That's 1 master + 18 variants.

## How the pieces combine into moods

| Mood / state | Brows | Eyes | Mouth |
|---|---|---|---|
| Idle | neutral | open, blinks with `eyes_closed`, glances with `eyes_left` / `eyes_right` | closed (master) |
| Happy | neutral | `eyes_happy` | `mouth_grin` |
| Excited | `brows_raised` | `eyes_wide` | `mouth_grin` |
| Curious | `brows_one_up` | open | `mouth_oo` |
| Sad | `brows_sad` | `eyes_half` | `mouth_frown` |
| Sleepy | neutral | `eyes_half` / `eyes_closed` | `mouth_flat` |
| Listening | `brows_raised` | `eyes_wide` | closed |
| Thinking | `brows_furrowed` | `eyes_up` | `mouth_flat` |
| Speaking | current mood | current mood | `talk_small` / `talk_mid` / `talk_wide` / `mouth_grin`, driven by voice volume |
| Confused (error) | `brows_furrowed` | open | `mouth_flat` |

## Tips

- **Getting a variant to change only one thing:** if ChatGPT keeps redrawing the whole image, use its edit/selection tool to paint over just the mouth (or eyes, or brows) and apply the variant line to that selection.
- **Keep the style locked:** if a variant drifts in style, say "match the master's line thickness and colors exactly."
- **Optional extras:** an `eyes_closed` + `mouth_flat` "asleep" combo needs no extra art. A tilted-head or waving pose would need its own full base image, so skip those for now.

## Building the assets

After adding or changing PNGs in `art/source/`:

```bash
art/.venv/bin/python art/build_assets.py
```

It prints each part's region and its "edge drift" (how much the variant differs from the master along the cut line). A warning there means the generator moved something, so regenerate that variant. Feature regions are in `REGIONS` at the top of the script; they're tied to this master's framing.
