/* Sets lines of text with Core Text and writes each as an 8-bit coverage
 * image — the text half of scripts/gen_intro_assets.py's caption atlases.
 *
 * Why not PIL, which draws everything else in those atlases: the captions are
 * in ten languages, one of them Arabic, and PIL without libraqm places glyphs
 * one after another — no joining, no right-to-left. Core Text shapes every
 * script, orders mixed directions, and falls back to a system face for a
 * script the caption faces do not have (they are Latin only).
 *
 *   intro_text_bake <jobs.tsv> <outdir>
 *
 * One job per line, tab-separated:
 *   id  font  cascade  size  tracking  language  text
 *     font      a font FILE (.ttf/.otf), "<file>@<wght>" for one weight of a
 *               variable font, or "name:<PostScript or family name>"
 *     cascade   more of the same, "|"-separated, tried in order for glyphs the
 *               font lacks ("-" for none); the system's own list comes after
 *     size      pixels
 *     tracking  pixels added after each glyph
 *     language  a BCP 47 tag: which regional forms a shared code point gets
 * Writes <outdir>/<id>.gray (width x height bytes, top row first) and prints
 *   id width height baseline advance fonts
 * where `baseline` is the baseline's distance from the image's top, `advance`
 * the line's typographic width, and `fonts` the faces actually used.
 *
 * Built and run by gen_intro_assets.py; not part of the app. */

#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>
#import <Foundation/Foundation.h>

static const CGFloat kPad = 6.0;

/* "name:<font name>", or a font file, or "<font file>@<wght>" for one instance
 * of a variable font. */
static CTFontDescriptorRef descriptor_for(NSString *spec, CGFloat size) {
    if ([spec hasPrefix:@"name:"]) {
        return CTFontDescriptorCreateWithNameAndSize((__bridge CFStringRef)[spec substringFromIndex:5], size);
    }
    NSString *path = spec, *weight = nil;
    NSRange at = [spec rangeOfString:@"@" options:NSBackwardsSearch];
    if (at.location != NSNotFound) {
        path = [spec substringToIndex:at.location];
        weight = [spec substringFromIndex:at.location + 1];
    }
    NSURL *url = [NSURL fileURLWithPath:path];
    CFArrayRef all = CTFontManagerCreateFontDescriptorsFromURL((__bridge CFURLRef)url);
    if (!all || CFArrayGetCount(all) == 0) {
        if (all) CFRelease(all);
        return NULL;
    }
    CTFontDescriptorRef first = CFRetain(CFArrayGetValueAtIndex(all, 0));
    CFRelease(all);
    if (!weight) return first;
    NSDictionary *variation = @{ (id)kCTFontVariationAttribute : @{ @(0x77676874) : @(weight.doubleValue) } };   /* 'wght' */
    CTFontDescriptorRef instance = CTFontDescriptorCreateCopyWithAttributes(first, (__bridge CFDictionaryRef)variation);
    CFRelease(first);
    return instance;
}

int main(int argc, char **argv) {
    @autoreleasepool {
        if (argc != 3) {
            fprintf(stderr, "usage: intro_text_bake <jobs.tsv> <outdir>\n");
            return 2;
        }
        NSString *jobs = [NSString stringWithContentsOfFile:@(argv[1]) encoding:NSUTF8StringEncoding error:NULL];
        NSString *outdir = @(argv[2]);
        if (!jobs) { fprintf(stderr, "cannot read %s\n", argv[1]); return 1; }

        for (NSString *line in [jobs componentsSeparatedByString:@"\n"]) {
            if (line.length == 0) continue;
            NSArray<NSString *> *f = [line componentsSeparatedByString:@"\t"];
            if (f.count != 7) { fprintf(stderr, "bad job: %s\n", line.UTF8String); return 1; }
            NSString *ident = f[0], *text = f[6];
            CGFloat size = f[3].doubleValue, tracking = f[4].doubleValue;

            CTFontDescriptorRef base = descriptor_for(f[1], size);
            if (!base) { fprintf(stderr, "%s: no font %s\n", ident.UTF8String, f[1].UTF8String); return 1; }
            NSMutableDictionary *attributes = [NSMutableDictionary dictionary];
            if (![f[2] isEqualToString:@"-"]) {
                NSMutableArray *cascade = [NSMutableArray array];
                for (NSString *spec in [f[2] componentsSeparatedByString:@"|"]) {
                    CTFontDescriptorRef d = descriptor_for(spec, size);
                    if (!d) { fprintf(stderr, "%s: no fallback font %s\n", ident.UTF8String, spec.UTF8String); return 1; }
                    [cascade addObject:(__bridge_transfer id)d];
                }
                attributes[(id)kCTFontCascadeListAttribute] = cascade;
            }
            CTFontDescriptorRef full = CTFontDescriptorCreateCopyWithAttributes(base, (__bridge CFDictionaryRef)attributes);
            CTFontRef font = CTFontCreateWithFontDescriptor(full, size, NULL);
            CFRelease(full);
            CFRelease(base);

            NSDictionary *run = @{
                (id)kCTFontAttributeName : (__bridge id)font,
                (id)kCTKernAttributeName : @(tracking),
                (id)kCTLanguageAttributeName : f[5],
                (id)kCTForegroundColorFromContextAttributeName : @YES,
            };
            NSAttributedString *string = [[NSAttributedString alloc] initWithString:text attributes:run];
            CTLineRef set = CTLineCreateWithAttributedString((__bridge CFAttributedStringRef)string);
            CGFloat ascent = 0, descent = 0, leading = 0;
            /* The tracking after the last glyph is not part of the line. */
            CGFloat advance = CTLineGetTypographicBounds(set, &ascent, &descent, &leading) - tracking;

            size_t width = (size_t)ceil(advance + 2.0 * kPad), height = (size_t)ceil(ascent + descent + 2.0 * kPad);
            NSMutableData *pixels = [NSMutableData dataWithLength:width * height];
            CGColorSpaceRef gray = CGColorSpaceCreateDeviceGray();
            CGContextRef ctx = CGBitmapContextCreate(pixels.mutableBytes, width, height, 8, width, gray,
                                                     (CGBitmapInfo)kCGImageAlphaNone);
            CGColorSpaceRelease(gray);
            if (!ctx) { fprintf(stderr, "%s: no bitmap\n", ident.UTF8String); return 1; }
            CGContextSetGrayFillColor(ctx, 1.0, 1.0);
            CGContextSetShouldAntialias(ctx, true);
            /* Grey coverage, not LCD fringes and not the "smoothing" that
             * fattens stems: this is a mask the shader tints. */
            CGContextSetShouldSmoothFonts(ctx, false);
            CGContextSetAllowsFontSubpixelPositioning(ctx, true);
            CGContextSetShouldSubpixelPositionFonts(ctx, true);
            /* The bitmap's origin is its bottom-left; its bytes are top row first. */
            CGContextSetTextPosition(ctx, kPad, kPad + descent);
            CTLineDraw(set, ctx);
            CGContextRelease(ctx);

            NSMutableOrderedSet *used = [NSMutableOrderedSet orderedSet];
            for (id glyphRun in (__bridge NSArray *)CTLineGetGlyphRuns(set)) {
                NSDictionary *a = (__bridge NSDictionary *)CTRunGetAttributes((__bridge CTRunRef)glyphRun);
                CTFontRef runFont = (__bridge CTFontRef)a[(id)kCTFontAttributeName];
                [used addObject:(__bridge_transfer NSString *)CTFontCopyPostScriptName(runFont)];
            }
            CFRelease(set);
            CFRelease(font);

            NSString *path = [outdir stringByAppendingPathComponent:[ident stringByAppendingString:@".gray"]];
            if (![pixels writeToFile:path atomically:NO]) { fprintf(stderr, "cannot write %s\n", path.UTF8String); return 1; }
            printf("%s %zu %zu %.3f %.3f %s\n", ident.UTF8String, width, height, (double)(kPad + ascent), (double)advance,
                   [[used array] componentsJoinedByString:@","].UTF8String);
        }
    }
    return 0;
}
