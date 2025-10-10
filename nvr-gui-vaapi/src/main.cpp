#include <QApplication>
#include <QWidget>
#include <QGridLayout>
#include <QLabel>
#include <QImage>
#include <QPixmap>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QElapsedTimer>
#include <atomic>
#include <thread>
#include <vector>
#include <algorithm>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

static AVBufferRef *g_hw_device_ctx = nullptr;
static bool init_hw_device() {
    int err = av_hwdevice_ctx_create(&g_hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI,
                                     "/dev/dri/renderD128", nullptr, 0);
    if (err < 0) {
        fprintf(stderr, "[VAAPI] ? Tidak bisa membuat context VAAPI (%d)\n", err);
        return false;
    }
    fprintf(stderr, "[VAAPI] ? VAAPI aktif di /dev/dri/renderD128 (satu global)\n");
    return true;
}

static enum AVPixelFormat get_hw_format(AVCodecContext *ctx, const enum AVPixelFormat *pix_fmts) {
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; p++)
        if (*p == AV_PIX_FMT_VAAPI) return *p;
    return pix_fmts[0];
}

// ================================================================
struct RtspWorker {
    QString nama, url;
    QLabel *target{};
    int outW{960}, outH{540};
    std::atomic<bool> jalan{false};
    std::thread th;
    bool pakaiVAAPI{false};

    void mulai() { jalan = true; th = std::thread([&] { proses(); }); }
    void berhenti(){ jalan=false; if(th.joinable()) th.join(); }

    void proses() {
        avformat_network_init();
        fprintf(stderr, "\n[RTSP] ?? Memulai kamera \"%s\"\n", nama.toUtf8().constData());

        AVFormatContext *fmt=nullptr; AVDictionary *opts=nullptr;
        av_dict_set(&opts,"rtsp_transport","tcp",0);
        av_dict_set(&opts,"stimeout","5000000",0);
        av_dict_set(&opts,"fflags","+genpts",0);
        av_dict_set(&opts,"use_wallclock_as_timestamps","1",0);

        if(avformat_open_input(&fmt,url.toUtf8().constData(),nullptr,&opts)<0){
            fprintf(stderr,"[RTSP] ? Tidak bisa membuka stream %s\n",nama.toUtf8().constData());
            return;
        }
        avformat_find_stream_info(fmt,nullptr);
        int vIdx=av_find_best_stream(fmt,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
        if(vIdx<0){ fprintf(stderr,"[RTSP] ? Tidak ada stream video %s\n",nama.toUtf8().constData()); return; }

        AVStream *vs=fmt->streams[vIdx];
        const AVCodec *dec=avcodec_find_decoder(vs->codecpar->codec_id);
        AVCodecContext *ctx=avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(ctx,vs->codecpar);

        if(pakaiVAAPI && g_hw_device_ctx){
            ctx->get_format=get_hw_format;
            ctx->hw_device_ctx=av_buffer_ref(g_hw_device_ctx);
            fprintf(stderr,"[VAAPI] ?? Kamera \"%s\" pakai VAAPI\n",nama.toUtf8().constData());
        }else{
            fprintf(stderr,"[RTSP] ?? Kamera \"%s\" pakai decoding CPU\n",nama.toUtf8().constData());
        }

        if(avcodec_open2(ctx,dec,nullptr)<0){
            fprintf(stderr,"[RTSP] ? Gagal membuka decoder %s\n",nama.toUtf8().constData());
            return;
        }

        AVPacket *pkt=av_packet_alloc();
        AVFrame *frm=av_frame_alloc(), *swfrm=av_frame_alloc();
        SwsContext *sws=nullptr;
        QImage img(outW,outH,QImage::Format_RGB32);
        QElapsedTimer tmr; tmr.start(); int hitung=0; bool tampil=false;

        while(jalan){
            if(av_read_frame(fmt,pkt)<0) continue;
            if(pkt->stream_index!=vIdx){ av_packet_unref(pkt); continue; }
            if(avcodec_send_packet(ctx,pkt)==0){
                while(jalan){
                    int r=avcodec_receive_frame(ctx,frm);
                    if(r==AVERROR(EAGAIN)||r==AVERROR_EOF) break;
                    if(r<0) break;
                    av_frame_unref(swfrm);
                    if(pakaiVAAPI && frm->format==AV_PIX_FMT_VAAPI){
                        if(av_hwframe_transfer_data(swfrm,frm,0)<0){
                            fprintf(stderr,"[RTSP] ?? VAAPI gagal untuk \"%s\" ? fallback CPU\n",
                                    nama.toUtf8().constData());
                            pakaiVAAPI=false;
                            continue;
                        }
                    }else av_frame_ref(swfrm,frm);

                    if(!sws){
                        sws=sws_getContext(swfrm->width,swfrm->height,
                                           (AVPixelFormat)swfrm->format,
                                           outW,outH,AV_PIX_FMT_BGR0,
                                           SWS_BILINEAR,nullptr,nullptr,nullptr);
                        fprintf(stderr,"[RTSP] ?? Konverter %s (%dx%d?%dx%d)\n",
                                nama.toUtf8().constData(),swfrm->width,swfrm->height,outW,outH);
                    }

                    uint8_t *dst[4]={img.bits(),nullptr,nullptr,nullptr};
                    int dstL[4]={(int)img.bytesPerLine(),0,0,0};
                    sws_scale(sws,swfrm->data,swfrm->linesize,0,swfrm->height,dst,dstL);

                    QImage copy=img.copy();
                    QMetaObject::invokeMethod(target,[this,copy](){
                        target->setPixmap(QPixmap::fromImage(copy));
                    },Qt::QueuedConnection);

                    if(!tampil){
                        fprintf(stderr,"[RTSP] ? \"%s\" tampil di layar\n",nama.toUtf8().constData());
                        tampil=true;
                    }

                    hitung++;
                    if(tmr.elapsed()>3000){
                        double fps=hitung*1000.0/tmr.elapsed();
                        fprintf(stderr,"[RTSP] ?? \"%s\" %.1f FPS (%s)\n",
                                nama.toUtf8().constData(),fps,pakaiVAAPI?"VAAPI":"CPU");
                        tmr.restart(); hitung=0;
                    }
                    av_frame_unref(frm);
                }
            }
            av_packet_unref(pkt);
        }

        fprintf(stderr,"[RTSP] ? Kamera \"%s\" berhenti\n",nama.toUtf8().constData());
        if(sws) sws_freeContext(sws);
        av_frame_free(&frm); av_frame_free(&swfrm); av_packet_free(&pkt);
        avcodec_free_context(&ctx); avformat_close_input(&fmt);
    }
};

// ================================================================
struct SnapshotWorker {
    QString nama,url; QLabel *target{}; QNetworkAccessManager *net{};
    QElapsedTimer tmr; int frame{0};
    void mulai(){
        fprintf(stderr,"[HTTP] ?? Polling snapshot \"%s\"\n",nama.toUtf8().constData());
        tmr.start(); ambil();
    }
    void ambil(){
        QNetworkRequest req{QUrl(url)};
        QNetworkReply *rep=net->get(req);
        QObject::connect(rep,&QNetworkReply::finished,[&,rep](){
            QByteArray d=rep->readAll(); rep->deleteLater();
            QImage img; img.loadFromData(d,"JPG");
            if(!img.isNull()){
                target->setPixmap(QPixmap::fromImage(img.scaled(960,540)));
                frame++;
            }
            if(tmr.elapsed()>2000){
                double fps=frame*1000.0/tmr.elapsed();
                fprintf(stderr,"[HTTP] ?? \"%s\" %.1f FPS\n",nama.toUtf8().constData(),fps);
                frame=0; tmr.restart();
            }
            ambil();
        });
    }
};

// ================================================================
int main(int argc,char**argv){
    QApplication app(argc,argv);
    QString cfg=(argc>1)?argv[1]:"config.json";
    QFile f(cfg); if(!f.open(QIODevice::ReadOnly)){
        fprintf(stderr,"? Tidak menemukan file %s\n",cfg.toUtf8().constData());
        return 1;
    }
    QJsonArray cams=QJsonDocument::fromJson(f.readAll()).object()["cameras"].toArray();

    // tentukan kamera resolusi tertinggi
    struct Info{QString name,url;int width,height;};
    std::vector<Info> info;
    for(auto c:cams){
        QJsonObject o=c.toObject();
        info.push_back({o["name"].toString(),o["url"].toString(),0,0});
    }

    int maxIdx=0,maxRes=0;
    for(size_t i=0;i<info.size();++i){
        AVFormatContext*fmt=nullptr;AVDictionary*opt=nullptr;
        av_dict_set(&opt,"rtsp_transport","tcp",0);
        if(avformat_open_input(&fmt,info[i].url.toUtf8().constData(),nullptr,&opt)==0){
            avformat_find_stream_info(fmt,nullptr);
            int v=av_find_best_stream(fmt,AVMEDIA_TYPE_VIDEO,-1,-1,nullptr,0);
            if(v>=0){
                int w=fmt->streams[v]->codecpar->width;
                int h=fmt->streams[v]->codecpar->height;
                info[i].width=w; info[i].height=h;
                int res=w*h;
                if(res>maxRes){maxRes=res;maxIdx=i;}
            }
            avformat_close_input(&fmt);
        }
    }

    fprintf(stderr,"[AUTO] Kamera VAAPI terpilih: \"%s\" (%dx%d)\n",
            info[maxIdx].name.toUtf8().constData(),
            info[maxIdx].width,info[maxIdx].height);
    init_hw_device();

    // ================================================================
    // Layout 2x2 fullscreen tanpa ruang kosong
    // ================================================================
    QWidget win;
    win.setWindowTitle("NVR 4-Channel Fullscreen");
    win.resize(1920,1080);
    win.setStyleSheet("background:black;");
    QGridLayout grid(&win);
    grid.setSpacing(0);
    grid.setContentsMargins(0,0,0,0);
    grid.setRowStretch(0,1); grid.setRowStretch(1,1);
    grid.setColumnStretch(0,1); grid.setColumnStretch(1,1);

    std::vector<QLabel*> tiles(4);
    for(int i=0;i<4;i++){
        tiles[i]=new QLabel(&win);
        tiles[i]->setAlignment(Qt::AlignCenter);
        tiles[i]->setStyleSheet(
            "background:black;"
            "border:1px solid #111;"
            "color:#888;"
            "font-size:18px;");
        tiles[i]->setText("Loading...");
        tiles[i]->setScaledContents(true);
        grid.addWidget(tiles[i],i/2,i%2);
    }
    win.showFullScreen(); // fullscreen otomatis

    std::vector<RtspWorker> rtsp(3);
    QNetworkAccessManager net;
    for(int i=0;i<3;i++){
        rtsp[i].nama=info[i].name; rtsp[i].url=info[i].url; rtsp[i].target=tiles[i];
        rtsp[i].pakaiVAAPI=(i==maxIdx); rtsp[i].mulai();
    }

    SnapshotWorker snap;
    snap.nama=info[3].name; snap.url=info[3].url;
    snap.target=tiles[3]; snap.net=&net; snap.mulai();

    int rc=app.exec();
    for(auto&w:rtsp) w.berhenti();
    if(g_hw_device_ctx) av_buffer_unref(&g_hw_device_ctx);
    return rc;
}
