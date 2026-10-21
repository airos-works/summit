#!/usr/bin/env python3
"""Check screenshots of tests/fixtures/composited-filters.html with Pillow.

Capture the backdrop, blur and foreground cases using MCP screenshot with
target=page and max_width=0. Capture the blur case with page_render too.
Screen pixels must correspond to CSS pixels; page_render may be at a higher
device scale. Backdrop snapshots are not a software reference: WebCore's
snapshotRect does not paint the compositor's backdrop pass.
"""
import argparse
import json
from PIL import Image, ImageChops, ImageStat


def check(backdrop, blur, reference, foreground):
    backdrop, blur, reference, foreground = [Image.open(p).convert('RGB')
        for p in (backdrop, blur, reference, foreground)]
    assert backdrop.size == blur.size == foreground.size
    assert blur.width >= 480 and blur.height >= 420
    reference = reference.resize(blur.size, Image.Resampling.BOX)
    glass = ImageStat.Stat(backdrop.crop((160, 150, 240, 190)))
    sharp = backdrop.crop((40, 80, 400, 120)).getextrema()
    error = ImageStat.Stat(ImageChops.difference(blur.crop((20, 40, 420, 380)),
        reference.crop((20, 40, 420, 380))))
    # A later blue sibling must not become part of the earlier backdrop's input.
    later = ImageStat.Stat(ImageChops.difference(backdrop.crop((200, 145, 248, 195)),
        foreground.crop((200, 145, 248, 195))))
    samples = [blur.getpixel((x, 190))[1] for x in (40, 80, 120, 140, 220, 300, 340, 400)]
    result = {'glass_mean': glass.mean, 'glass_stddev': glass.stddev,
        'unfiltered_ranges': sharp, 'blur_mean_absolute_error': error.mean,
        'blur_max_error': error.extrema, 'blur_green_samples': samples,
        'foreground_contamination': later.extrema}
    print(json.dumps(result, indent=2))
    # With 20px sigma over 16px alternating black/white stripes, the interior
    # is almost uniform grey. The 10% white overlay raises 127.5 to 140.25.
    assert all(abs(mean - 140.25) < 12 for mean in glass.mean), 'backdrop mean'
    assert max(glass.stddev) < 12, 'backdrop left stripes sharp'
    assert all(hi - lo > 240 for lo, hi in sharp), 'unfiltered content was blurred'
    assert max(error.mean) < 5 and max(hi for lo, hi in error.extrema) < 20, 'blur differs from software'
    assert samples[0] == samples[-1] == 255, 'blur does not fade to white'
    assert samples[1] > samples[2] > samples[3] > samples[4], 'blur has a hard left edge'
    assert samples[4] < samples[5] < samples[6] < samples[7], 'blur has a hard right edge'
    assert foreground.getpixel((270, 170)) == (0, 0, 255), 'foreground missing'
    assert max(hi for lo, hi in later.extrema) <= 2, 'later sibling leaked into backdrop'
    print('PASS: backdrop, blur outsets and foreground ordering')


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('backdrop', 'blur', 'reference', 'foreground'):
        parser.add_argument('--' + name, required=True)
    args = parser.parse_args()
    check(args.backdrop, args.blur, args.reference, args.foreground)
