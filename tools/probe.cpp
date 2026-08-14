/*
 * bencsnip-probe - what the media layer makes of a file
 *
 * Not a feature of the editor; a way to answer "is this the decoder or is it
 * the GUI" without a window. Prints what probe() found, writes the frame the
 * bin would use as a thumbnail to a .ppm, and pulls a second of audio through
 * the same path the mixer uses.
 *
 *   bencsnip-probe FILE [-t SECONDS] [-o out.ppm]
 */

#include "sn_media.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void write_ppm(const char *path, const sn::VideoFrame &f)
{
    FILE *o = fopen(path, "wb");
    if (!o) { fprintf(stderr, "cannot write %s\n", path); return; }
    fprintf(o, "P6\n%d %d\n255\n", f.w, f.h);
    for (size_t i = 0; i < f.rgba.size(); i += 4)
        fwrite(&f.rgba[i], 1, 3, o);
    fclose(o);
    printf("  wrote %s (%dx%d)\n", path, f.w, f.h);
}

int main(int argc, char **argv)
{
    const char *path = nullptr, *out = nullptr;
    double at = -1;

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-t") && i + 1 < argc) at = atof(argv[++i]);
        else if (!strcmp(argv[i], "-o") && i + 1 < argc) out = argv[++i];
        else path = argv[i];
    }
    if (!path) { fprintf(stderr, "usage: bencsnip-probe FILE [-t SEC] [-o out.ppm]\n"); return 2; }

    std::string err;
    sn::MediaInfo mi;
    if (!sn::probe(path, &mi, &err)) { fprintf(stderr, "%s\n", err.c_str()); return 1; }

    printf("%s\n", mi.name.c_str());
    printf("  container   %s\n", mi.container.c_str());
    printf("  duration    %s (%.3f s)\n", sn::fmtTime(mi.duration).c_str(), mi.duration);
    printf("  size        %s\n", sn::fmtSize(mi.bytes).c_str());
    if (mi.hasVideo)
        printf("  video       %s %dx%d  %.3f fps  sar %.4f  rot %d  -> display %dx%d\n",
               mi.vcodec.c_str(), mi.width, mi.height, mi.fps, mi.sar, mi.rotation,
               mi.dispW(), mi.dispH());
    else printf("  video       none\n");
    if (mi.hasAudio)
        printf("  audio       %s  %d Hz  %d ch\n", mi.acodec.c_str(), mi.rate, mi.chans);
    else printf("  audio       none\n");

    sn::Source *s = sn::Source::open(path, &err);
    if (!s) { fprintf(stderr, "%s\n", err.c_str()); return 1; }

    if (s->hasVideo()) {
        double t = at >= 0 ? at : (mi.duration > 1 ? mi.duration * 0.1 : 0.0);
        sn::VideoFrame f;
        if (s->frameAt(t, &f, 0, 0))
            printf("  frame at %.3f -> pts %.3f  %dx%d\n", t, f.pts, f.w, f.h);
        else
            printf("  frame at %.3f -> nothing\n", t);
        if (out && f.valid()) write_ppm(out, f);
    }

    if (s->hasAudio()) {
        double t = at >= 0 ? at : 0.0;
        const int block = 1024;
        std::vector<float> buf(block * sn::CHANS);
        double peak = 0, sum = 0;
        int blocks = sn::RATE / block;      /* about a second */
        for (int i = 0; i < blocks; i++) {
            s->audioAt(t + i * block / (double)sn::RATE, block, buf.data());
            for (float v : buf) { peak = std::fmax(peak, std::fabs(v)); sum += v * v; }
        }
        double rms = std::sqrt(sum / (blocks * block * sn::CHANS));
        printf("  audio from %.3f: peak %.3f  rms %.4f  (%.1f dBFS)\n",
               t, peak, rms, 20.0 * std::log10(rms > 0 ? rms : 1e-9));
    }

    delete s;
    return 0;
}
