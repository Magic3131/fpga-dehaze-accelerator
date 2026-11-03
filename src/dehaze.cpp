// Advanced inspection HLS core. RGB888, TUSER=SOF, TLAST=EOL.
// Old ctrl bits 0..2 and threshold bytes retain their exact meanings.
// No full-frame RAM, no dynamic allocation, no pixel-domain temporal averaging.
#include "dehaze.hpp"
using namespace inspection;
typedef ap_uint<24> rgb_t;
typedef ap_uint<32> window_t; // [31:24] local dark channel, [23:0] center RGB

#define K(i) (unsigned short)((65280u + (i)/2u)/(i))
#define A(i) (unsigned short)((55552u + (i)/2u)/(i))
#define SIXTEEN(F,n) F(n),F(n+1),F(n+2),F(n+3),F(n+4),F(n+5),F(n+6),F(n+7),F(n+8),F(n+9),F(n+10),F(n+11),F(n+12),F(n+13),F(n+14),F(n+15)
#define TABLE(F) 0,SIXTEEN(F,1),SIXTEEN(F,17),SIXTEEN(F,33),SIXTEEN(F,49),SIXTEEN(F,65),SIXTEEN(F,81),SIXTEEN(F,97),SIXTEEN(F,113),SIXTEEN(F,129),SIXTEEN(F,145),SIXTEEN(F,161),SIXTEEN(F,177),SIXTEEN(F,193),SIXTEEN(F,209),SIXTEEN(F,225),F(241),F(242),F(243),F(244),F(245),F(246),F(247),F(248),F(249),F(250),F(251),F(252),F(253),F(254),F(255)
static const unsigned short reciprocal_t[] = {TABLE(K)};
static const unsigned short reciprocal_a[] = {TABLE(A)};
static_assert(sizeof(reciprocal_t)/sizeof(unsigned short)==256,"reciprocal table");
static_assert(sizeof(reciprocal_a)/sizeof(unsigned short)==256,"reciprocal table");
#undef TABLE
#undef SIXTEEN
#undef A
#undef K

static ap_uint<8> min3(ap_uint<8> a, ap_uint<8> b, ap_uint<8> c) {
#pragma HLS INLINE
    ap_uint<8> m = a < b ? a : b;
    return m < c ? m : c;
}
static ap_uint<8> minch(rgb_t p) {
#pragma HLS INLINE
    return min3(p.range(23,16),p.range(15,8),p.range(7,0));
}
static ap_uint<8> maxch(rgb_t p) {
#pragma HLS INLINE
    ap_uint<8> r=p.range(23,16),g=p.range(15,8),b=p.range(7,0);
    ap_uint<8> m=r>g?r:g;
    return m>b?m:b;
}
static ap_uint<8> recover(ap_uint<8> c,ap_uint<8> a,ap_uint<16> gain) {
#pragma HLS INLINE
    ap_int<10> d=(ap_int<10>)c-(ap_int<10>)a;
    ap_int<26> product=d*(ap_int<17>)gain;
    ap_int<19> value=(ap_int<19>)a+(ap_int<19>)(product>>8);
    return value<0 ? (ap_uint<8>)0 : (value>255 ? (ap_uint<8>)255 : (ap_uint<8>)value);
}

// One scanner covers all borders. First row initializes both line memories;
// row HEIGHT and column WIDTH are virtual BORDER_REPLICATE samples.
static void dark_window(hls::stream<rgb_t>& input,
                        hls::stream<window_t>& output,ap_uint<32> ctrl) {
#pragma HLS INLINE off
    if (!ctrl[0]) {
    WINDOW_BYPASS: for (unsigned n=0;n<WIDTH*HEIGHT;++n) {
#pragma HLS PIPELINE II=1
            output.write((window_t)input.read());
        }
        return;
    }
    rgb_t line_a[WIDTH],line_b[WIDTH];
#pragma HLS BIND_STORAGE variable=line_a type=ram_s2p impl=bram
#pragma HLS BIND_STORAGE variable=line_b type=ram_s2p impl=bram
    ap_uint<8> w[3][3];
#pragma HLS ARRAY_PARTITION variable=w complete dim=0
    ap_uint<8> last[3]={0,0,0};
#pragma HLS ARRAY_PARTITION variable=last complete
    rgb_t delayed=0,last_middle=0;
    WINDOW_ROWS: for (unsigned y=0;y<=HEIGHT;++y) {
        WINDOW_COLS: for (unsigned x=0;x<=WIDTH;++x) {
#pragma HLS PIPELINE II=1
            rgb_t middle=last_middle;
            ap_uint<8> m[3];
#pragma HLS ARRAY_PARTITION variable=m complete
            if (x<WIDTH) {
                rgb_t current=0,older=0;
                if (y<HEIGHT) current=input.read();
                if (y==0) {
                    middle=current; older=current;
                    line_a[x]=current;line_b[x]=current;
                } else {
                    rgb_t va=line_a[x],vb=line_b[x];
                    middle=(y&1)?va:vb;
                    older=(y&1)?vb:va;
                    if (y<HEIGHT) {
                        if (y&1) line_b[x]=current;
                        else line_a[x]=current;
                    } else current=middle;
                }
                m[0]=minch(older);m[1]=minch(middle);m[2]=minch(current);
                last_middle=middle;
                for (unsigned r=0;r<3;++r) {
#pragma HLS UNROLL
                    last[r]=m[r];
                }
            } else {
                for (unsigned r=0;r<3;++r) {
#pragma HLS UNROLL
                    m[r]=last[r];
                }
            }
            for (unsigned r=0;r<3;++r) {
#pragma HLS UNROLL
                if (x==0) {w[r][0]=m[r];w[r][1]=m[r];w[r][2]=m[r];}
                else {w[r][0]=w[r][1];w[r][1]=w[r][2];w[r][2]=m[r];}
            }
            if (y>0 && x>0) {
                ap_uint<8> dark=min3(min3(w[0][0],w[0][1],w[0][2]),
                                    min3(w[1][0],w[1][1],w[1][2]),
                                    min3(w[2][0],w[2][1],w[2][2]));
                window_t packet=delayed;
                packet.range(31,24)=dark;
                output.write(packet);
            }
            delayed=middle;
        }
    }
}

// Statistics operate on original center RGB, at a 16x16 sample spacing.
// Estimate is a bounded neutral-brightness percentile, not a haze detector.
// It excludes saturated pixels; snow filling the image can still bias it.
static void restore_stream(hls::stream<window_t>& input,
                           hls::stream<rgb_t>& output,
                           ap_uint<32> ctrl,ap_uint<32> params) {
#pragma HLS INLINE off
    static ap_uint<8> previous_a=DEFAULT_A;
    static bool valid_a=false;
#pragma HLS RESET variable=previous_a
#pragma HLS RESET variable=valid_a
    const ap_uint<8> seed=byte_default(params,16,DEFAULT_A);
    const bool automatic=ctrl[4];
    if (!automatic || ctrl[5] || !valid_a) previous_a=seed;
    valid_a=automatic && ctrl[0];
    if (!ctrl[0]) {
    RESTORE_BYPASS: for (unsigned n=0;n<WIDTH*HEIGHT;++n) {
#pragma HLS PIPELINE II=1
            output.write((rgb_t)input.read());
        }
        return;
    }
    ap_uint<8> a=automatic ? previous_a : seed;
    ap_uint<8> tmin=byte_default(params,8,DEFAULT_TMIN);
    ap_uint<8> omega=byte_default(params,0,DEFAULT_OMEGA);
    ap_uint<17> scale=(reciprocal_a[(unsigned)a]*(unsigned)omega+108u)/217u;
    ap_uint<16> limit=gain_q4(ctrl)<<4;
    ap_uint<8> bright=bright_min(ctrl),chroma=chroma_max(ctrl);
    ap_uint<16> hist[16];
#pragma HLS ARRAY_PARTITION variable=hist complete
    for (unsigned i=0;i<16;++i) {
#pragma HLS UNROLL
        hist[i]=0;
    }
    ap_uint<16> count=0;
    RESTORE_ROWS: for (unsigned y=0;y<HEIGHT;++y) {
        RESTORE_COLS: for (unsigned x=0;x<WIDTH;++x) {
#pragma HLS PIPELINE II=1
            window_t packet=input.read();
            rgb_t rgb=packet.range(23,0);
            ap_uint<8> dark=packet.range(31,24),lo=minch(rgb),hi=maxch(rgb);
            ap_uint<8> spread=hi-lo;
            if (automatic && (x&15)==0 && (y&15)==0 &&
                lo>=128 && hi<250 && spread<=48) {
                ap_uint<4> bin=lo>>4;
                hist[bin]=hist[bin]+1;
                count=count+1;
            }
            ap_uint<25> attenuation=dark*scale;
            ap_int<18> raw=255-(ap_int<18>)(attenuation>>8);
            ap_uint<8> t=raw<(ap_int<18>)tmin?tmin:(raw>255?(ap_uint<8>)255:(ap_uint<8>)raw);
            ap_uint<16> gain=reciprocal_t[(unsigned)t];
            if (ctrl[6] && gain>limit) gain=limit;
            // Preserve near-white regions and clipped highlights exactly.
            // This is a configurable protection heuristic, not event recognition.
            if (ctrl[3] && ((lo>=bright && spread<=chroma) || hi>=250)) gain=256;
            rgb_t restored;
            restored.range(23,16)=recover(rgb.range(23,16),a,gain);
            restored.range(15,8)=recover(rgb.range(15,8),a,gain);
            restored.range(7,0)=recover(rgb.range(7,0),a,gain);
            output.write(restored);
        }
    }
    if (automatic && count>=16) {
        unsigned rank=((unsigned)count*9u+9u)/10u,acc=0,target=200;
        bool found=false;
        for (unsigned bin=0;bin<16;++bin) {
            acc+=(unsigned)hist[bin];
            if (!found && acc>=rank) {target=bin*16+8;found=true;}
        }
        if (target<160) target=160;
        if (target>240) target=240;
        int delta=(int)target-(int)a;
        int step=delta>=0?(delta+3)/4:-((-delta+3)/4);
        if (step>4) step=4;
        if (step<-4) step=-4;
        previous_a=(ap_uint<8>)((int)a+step);
    }
}

static void dehaze_core(hls::stream<rgb_t>& input,hls::stream<rgb_t>& output,
                        ap_uint<32> ctrl,ap_uint<32> params) {
#pragma HLS INLINE off
    hls::stream<window_t> window("dark_to_restore");
#pragma HLS STREAM variable=window depth=64
#pragma HLS BIND_STORAGE variable=window type=fifo impl=lutram
#pragma HLS DATAFLOW
    dark_window(input,window,ctrl);
    restore_stream(window,output,ctrl,params);
}

inline void write_rgb_pixel(hls::stream<rgb_t>& stream, rgb_t pixel) {
#pragma HLS INLINE
    stream.write(pixel);
}
inline ap_uint<8> rgb_to_gray(ap_uint<24> rgb) {
#pragma HLS INLINE
    ap_uint<8> r=(ap_uint<8>)rgb.range(23,16);
    ap_uint<8> g=(ap_uint<8>)rgb.range(15, 8);
    ap_uint<8> b=(ap_uint<8>)rgb.range( 7, 0);
    ap_uint<17> sum=(ap_uint<17>)77*r+(ap_uint<17>)150*g
                    +(ap_uint<17>)29*b+128;
    return (ap_uint<8>)(sum >> 8);
}

inline ap_uint<8> sobel_magnitude(ap_uint<8> a,ap_uint<8> b,ap_uint<8> c,
                                  ap_uint<8> d,ap_uint<8> f,ap_uint<8> g,
                                  ap_uint<8> h,ap_uint<8> i) {
#pragma HLS INLINE
    ap_int<12> gx=(int)c+2*(int)f+(int)i-(int)a-2*(int)d-(int)g;
    ap_int<12> gy=(int)g+2*(int)h+(int)i-(int)a-2*(int)b-(int)c;
    if (gx<0) gx=-gx;
    if (gy<0) gy=-gy;
    ap_uint<12> mag=(ap_uint<12>)gx+(ap_uint<12>)gy;
    if (mag>255) mag=255;
    return (ap_uint<8>)mag;
}

inline ap_uint<24> sobel_result(ap_uint<24> base,ap_uint<8> edge,
                                ap_uint<8> edge_threshold,bool overlay) {
#pragma HLS INLINE
    bool is_edge = edge >= edge_threshold;
    if (overlay) return is_edge ? (ap_uint<24>)0xFFFFFF : base;
    ap_uint<8> v = is_edge ? edge : (ap_uint<8>)0;
    return ((ap_uint<24>)v<<16)|((ap_uint<24>)v<<8)|(ap_uint<24>)v;
}

void sobel_core(hls::stream<ap_uint<24> >& in_stream,
                hls::stream<ap_uint<24> >& out_stream,
                ap_uint<32> ctrl_reg, ap_uint<32> threshold_reg) {
#pragma HLS INLINE off
    const bool enabled=ctrl_reg[1];
    const bool overlay=ctrl_reg[2];
    ap_uint<8> edge_raw=(ap_uint<8>)threshold_reg.range(31,24);
    ap_uint<8> edge_threshold=edge_raw ? edge_raw
                                        : (ap_uint<8>)DEFAULT_EDGE;
    if (!enabled) {
        for (int n=0;n<WIDTH*HEIGHT;++n) {
#pragma HLS PIPELINE II=1
            write_rgb_pixel(out_stream,in_stream.read());
        }
        return;
    }

    ap_uint<8> lineA[WIDTH];
    ap_uint<8> lineB[WIDTH];
    ap_uint<24> rgbLine[WIDTH];
    for (int c=0;c<WIDTH;++c) {
        lineA[c]=0; lineB[c]=0; rgbLine[c]=0;
    }
    ap_uint<8> w0a=0,w0b=0,w0c=0;
    ap_uint<8> w1a=0,w1b=0,w1c=0;
    ap_uint<8> w2a=0,w2b=0,w2c=0;
    ap_uint<8> last0=0,last1=0,last2=0;
    ap_uint<24> delayed_rgb=0;

    for (int row=0;row<HEIGHT;++row) {
        const bool odd=(row&1)!=0;
        const bool wrA=!odd;
        const bool wrB=odd || row==0;
        for (int col=0;col<WIDTH;++col) {
#pragma HLS PIPELINE II=1
            ap_uint<24> rgb=in_stream.read();
            ap_uint<24> old_rgb=rgbLine[col];
            rgbLine[col]=rgb;
            ap_uint<8> current=rgb_to_gray(rgb);
            ap_uint<8> prev1=odd ? lineA[col] : lineB[col];
            ap_uint<8> prev2=odd ? lineB[col] : lineA[col];
            if (wrA) lineA[col]=current;
            if (wrB) lineB[col]=current;
            ap_uint<8> m2=current,m1=prev1,m0=prev2;
            last0=m0; last1=m1; last2=m2;
            if (col==0) {
                w0a=m0;w0b=m0;w0c=m0;
                w1a=m1;w1b=m1;w1c=m1;
                w2a=m2;w2b=m2;w2c=m2;
                delayed_rgb=old_rgb;
            } else {
                w0a=w0b;w0b=w0c;w0c=m0;
                w1a=w1b;w1b=w1c;w1c=m1;
                w2a=w2b;w2b=w2c;w2c=m2;
                ap_uint<8> edge=sobel_magnitude(w0a,w0b,w0c,
                                               w1a,w1c,
                                               w2a,w2b,w2c);
                if (row>0)
                    write_rgb_pixel(out_stream,
                        sobel_result(delayed_rgb,edge,edge_threshold,overlay));
                delayed_rgb=old_rgb;
            }
        }
        w0a=w0b;w0b=w0c;w0c=last0;
        w1a=w1b;w1b=w1c;w1c=last1;
        w2a=w2b;w2b=w2c;w2c=last2;
        ap_uint<8> edge=sobel_magnitude(w0a,w0b,w0c,
                                       w1a,w1c,
                                       w2a,w2b,w2c);
        if (row>0)
            write_rgb_pixel(out_stream,
                sobel_result(delayed_rgb,edge,edge_threshold,overlay));
    }

    const bool virtual_odd=(HEIGHT&1)!=0;
    for (int col=0;col<WIDTH;++col) {
#pragma HLS PIPELINE II=1
        ap_uint<24> base=rgbLine[col];
        ap_uint<8> prev1=virtual_odd ? lineA[col] : lineB[col];
        ap_uint<8> prev2=virtual_odd ? lineB[col] : lineA[col];
        ap_uint<8> m0=prev2,m1=prev1,m2=prev1;
        last0=m0;last1=m1;last2=m2;
        if (col==0) {
            w0a=m0;w0b=m0;w0c=m0;
            w1a=m1;w1b=m1;w1c=m1;
            w2a=m2;w2b=m2;w2c=m2;
            delayed_rgb=base;
        } else {
            w0a=w0b;w0b=w0c;w0c=m0;
            w1a=w1b;w1b=w1c;w1c=m1;
            w2a=w2b;w2b=w2c;w2c=m2;
            ap_uint<8> edge=sobel_magnitude(w0a,w0b,w0c,
                                           w1a,w1c,
                                           w2a,w2b,w2c);
            write_rgb_pixel(out_stream,
                sobel_result(delayed_rgb,edge,edge_threshold,overlay));
            delayed_rgb=base;
        }
    }
    w0a=w0b;w0b=w0c;w0c=last0;
    w1a=w1b;w1b=w1c;w1c=last1;
    w2a=w2b;w2b=w2c;w2c=last2;
    ap_uint<8> edge=sobel_magnitude(w0a,w0b,w0c,
                                   w1a,w1c,
                                   w2a,w2b,w2c);
    write_rgb_pixel(out_stream,
        sobel_result(delayed_rgb,edge,edge_threshold,overlay));
}





void dehaze(hls::stream<axis_pixel_t>& in_stream,
            hls::stream<axis_pixel_t>& out_stream,
            ap_uint<32> ctrl_reg,ap_uint<32> threshold_reg) {
#pragma HLS INTERFACE axis port=in_stream
#pragma HLS INTERFACE axis port=out_stream
#pragma HLS INTERFACE s_axilite port=ctrl_reg bundle=CTRL offset=0x10
#pragma HLS INTERFACE s_axilite port=threshold_reg bundle=CTRL offset=0x18
#pragma HLS INTERFACE s_axilite port=return bundle=CTRL
    hls::stream<rgb_t> rgb_input("axis_to_rgb"),dehazed("dehaze_to_sobel"),processed("sobel_to_axis");
#pragma HLS STREAM variable=rgb_input depth=64
#pragma HLS STREAM variable=dehazed depth=64
#pragma HLS STREAM variable=processed depth=64
#pragma HLS BIND_STORAGE variable=rgb_input type=fifo impl=lutram
#pragma HLS BIND_STORAGE variable=dehazed type=fifo impl=lutram
#pragma HLS BIND_STORAGE variable=processed type=fifo impl=lutram
#pragma HLS DATAFLOW
    for (unsigned n=0;n<WIDTH*HEIGHT;++n) {
#pragma HLS PIPELINE II=1
        axis_pixel_t p=in_stream.read();
        rgb_input.write(p.data);
    }
    dehaze_core(rgb_input,dehazed,ctrl_reg,threshold_reg);
    sobel_core(dehazed,processed,ctrl_reg,threshold_reg);
    for (unsigned n=0;n<WIDTH*HEIGHT;++n) {
#pragma HLS PIPELINE II=1
        axis_pixel_t p;
        p.data=processed.read();p.keep=7;p.strb=7;p.id=0;p.dest=0;
        p.user=(n==0);p.last=(n%WIDTH==WIDTH-1);
        out_stream.write(p);
    }
}
