// main.cpp versi final â€” log FPS menampilkan mode (vaapi/cpu)
// hybrid VAAPI hanya untuk kamera terberat, snapshot 10fps

#include <QApplication>
#include <QWidget>
#include <QGridLayout>
#include <QLabel>
#include <QPixmap>
#include <QImage>
#include <QFile>
#include <QFileInfo>
#include <QThread>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#include <QMetaObject>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrl>
#include <unistd.h>
#include <memory>
#include <vector>
#include <chrono>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/hwcontext.h>
#include <libswscale/swscale.h>
}

static AVBufferRef *g_hw_device_ctx = nullptr;
static std::atomic<bool> g_vaapi_already_assigned{false};

static enum AVPixelFormat get_hw_format(AVCodecContext *, const enum AVPixelFormat *pix_fmts) {
    for (const enum AVPixelFormat *p = pix_fmts; *p != AV_PIX_FMT_NONE; ++p)
        if (*p == AV_PIX_FMT_VAAPI) return *p;
    return pix_fmts[0];
}

static bool init_hw_device_once(const char *dev = "/dev/dri/renderD128") {
    if (g_hw_device_ctx) return true;
    int ret = av_hwdevice_ctx_create(&g_hw_device_ctx, AV_HWDEVICE_TYPE_VAAPI, dev, nullptr, 0);
    if (ret == 0) {
        fprintf(stderr, "[VAAPI] âœ… aktif di %s\n", dev);
        return true;
    }
    fprintf(stderr, "[VAAPI] âš ï¸ gagal init VAAPI (%d), pakai CPU.\n", ret);
    return false;
}

class HttpSnapshot : public QObject {
    Q_OBJECT
public:
    HttpSnapshot(const QString &name, const QString &url, QLabel *lbl, int w, int h, QObject *parent = nullptr)
        : QObject(parent), name_(name), url_(url), lbl_(lbl), w_(w), h_(h) {
        nam_ = new QNetworkAccessManager(this);
        timer_ = new QTimer(this);
        connect(timer_, &QTimer::timeout, this, &HttpSnapshot::tick);
        timer_->start(100);
        frameCount_ = 0;
        t0_ = std::chrono::steady_clock::now();
    }

public slots:
    void tick() {
        QNetworkRequest req{ QUrl(url_) };
        QNetworkReply *rep = nam_->get(req);
        connect(rep, &QNetworkReply::finished, this, [this, rep]() {
            if (rep->error() != QNetworkReply::NoError) {
                fprintf(stderr, "[HTTP] gagal ambil snapshot %s: %s\n",
                        name_.toLocal8Bit().constData(),
                        rep->errorString().toLocal8Bit().constData());
                rep->deleteLater();
                return;
            }
            QByteArray data = rep->readAll();
            rep->deleteLater();
            QImage img = QImage::fromData(data);
            if (!img.isNull()) {
                lbl_->setPixmap(QPixmap::fromImage(
                    img.scaled(w_, h_, Qt::KeepAspectRatio, Qt::FastTransformation)));
                frameCount_++;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0_).count() >= 10) {
                    double fps = frameCount_ / 10.0;
                    fprintf(stderr, "[FRAME] %s (snapshot) ~ %.2f fps\n",
                            name_.toLocal8Bit().constData(), fps);
                    frameCount_ = 0; t0_ = now;
                }
            }
        });
    }

private:
    QString name_, url_;
    QLabel *lbl_;
    int w_, h_;
    QNetworkAccessManager *nam_;
    QTimer *timer_;
    int frameCount_;
    std::chrono::steady_clock::time_point t0_;
};

class CameraWorker : public QThread {
    Q_OBJECT
public:
    CameraWorker(QString name, QString url, QLabel *label, int outW, int outH)
        : name_(std::move(name)), url_(std::move(url)), label_(label), outW_(outW), outH_(outH) {
        img_.reset(new QImage(outW_, outH_, QImage::Format_RGB32));
    }

    void run() override {
        avformat_network_init();
        AVFormatContext *fmt = nullptr;
        AVDictionary *opts = nullptr;
        av_dict_set(&opts, "rtsp_transport", "tcp", 0);
        av_dict_set(&opts, "fflags", "nobuffer", 0);
        if (avformat_open_input(&fmt, url_.toUtf8().constData(), nullptr, &opts) < 0) {
            fprintf(stderr, "[RTSP] âŒ gagal open %s\n", url_.toUtf8().constData());
            return;
        }
        av_dict_free(&opts);
        avformat_find_stream_info(fmt, nullptr);

        int vIdx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
        if (vIdx < 0) { fprintf(stderr, "[RTSP] âŒ tidak ada video: %s\n", name_.toUtf8().constData()); return; }

        AVStream *vs = fmt->streams[vIdx];
        const AVCodec *dec = avcodec_find_decoder(vs->codecpar->codec_id);
        AVCodecContext *ctx = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(ctx, vs->codecpar);
        ctx->skip_frame = AVDISCARD_NONREF;
        ctx->get_format = get_hw_format;

        use_vaapi_ = false;
        if ((vs->codecpar->width * vs->codecpar->height) >= (1280 * 720)
            && !g_vaapi_already_assigned.load() && init_hw_device_once()) {
            ctx->hw_device_ctx = av_buffer_ref(g_hw_device_ctx);
            use_vaapi_ = true;
            g_vaapi_already_assigned.store(true);
        }

        avcodec_open2(ctx, dec, nullptr);
        fprintf(stderr, "[RTSP] â–¶ï¸ %s mulai decode (%s, %dx%d, mode=%s)\n",
                name_.toUtf8().constData(), dec->name, ctx->width, ctx->height,
                use_vaapi_ ? "VAAPI" : "CPU");

        SwsContext *sws = nullptr;
        AVPacket *pkt = av_packet_alloc();
        AVFrame *frm = av_frame_alloc();
        AVFrame *swfrm = av_frame_alloc();

        int frameCount = 0;
        auto t0 = std::chrono::steady_clock::now();
        auto lastUiPush = std::chrono::steady_clock::now();

        while (!isInterruptionRequested()) {
            if (av_read_frame(fmt, pkt) < 0) { usleep(1000); continue; }
            if (pkt->stream_index != vIdx) { av_packet_unref(pkt); continue; }
            if (avcodec_send_packet(ctx, pkt) < 0) { av_packet_unref(pkt); continue; }
            av_packet_unref(pkt);

            while (avcodec_receive_frame(ctx, frm) == 0) {
                AVFrame *use = frm;
                if (use_vaapi_ && frm->format == AV_PIX_FMT_VAAPI) {
                    if (av_hwframe_transfer_data(swfrm, frm, 0) == 0)
                        use = swfrm;
                }
                if (!sws)
                    sws = sws_getContext(use->width, use->height, (AVPixelFormat)use->format,
                                         outW_, outH_, AV_PIX_FMT_BGR0, SWS_FAST_BILINEAR, nullptr, nullptr, nullptr);

                uint8_t *dst[4] = { img_->bits(), nullptr, nullptr, nullptr };
                int dstL[4] = { (int)img_->bytesPerLine(), 0, 0, 0 };
                sws_scale(sws, use->data, use->linesize, 0, use->height, dst, dstL);

                frameCount++;
                auto now = std::chrono::steady_clock::now();
                if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >= 10) {
                    fprintf(stderr, "[FRAME] %s ~ %.2f fps (%s)\n",
                            name_.toLocal8Bit().constData(), frameCount / 10.0,
                            use_vaapi_ ? "vaapi" : "cpu");
                    frameCount = 0; t0 = now;
                }

                double ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - lastUiPush).count();
                if (ms >= 90.0) {
                    QImage copy = img_->copy();
                    QMetaObject::invokeMethod(label_, [lbl = label_, copy]() {
                        lbl->setPixmap(QPixmap::fromImage(copy));
                    }, Qt::QueuedConnection);
                    lastUiPush = now;
                }
                av_frame_unref(frm);
                av_frame_unref(swfrm);
            }
        }

        if (sws) sws_freeContext(sws);
        av_frame_free(&frm); av_frame_free(&swfrm); av_packet_free(&pkt);
        avcodec_free_context(&ctx); avformat_close_input(&fmt);
    }

private:
    QString name_, url_;
    QLabel *label_;
    int outW_, outH_;
    std::unique_ptr<QImage> img_;
    bool use_vaapi_;
};

int main(int argc, char **argv) {
    QApplication app(argc, argv);

    QString cfgPath = "config.json";
    if (!QFile::exists(cfgPath)) {
        QFileInfo exeInfo(QString::fromLocal8Bit(argv[0]));
        QString alt = exeInfo.absolutePath() + "/config.json";
        if (QFile::exists(alt)) cfgPath = alt;
    }

    QFile f(cfgPath);
    if (!f.open(QIODevice::ReadOnly)) {
        fprintf(stderr, "config.json tidak ditemukan: %s\n", cfgPath.toLocal8Bit().constData());
        return 1;
    }

    QJsonDocument doc = QJsonDocument::fromJson(f.readAll());
    QJsonArray cams = doc.object().value("cameras").toArray();

    QWidget win;
    QGridLayout *grid = new QGridLayout(&win);
    grid->setSpacing(0);
    grid->setContentsMargins(0, 0, 0, 0);

    const int tileW = 960, tileH = 540;
    std::vector<CameraWorker *> workers;
    int idx = 0;

    for (auto v : cams) {
        QJsonObject o = v.toObject();
        QString name = o.value("name").toString();
        QString url = o.value("url").toString();

        if (url.startsWith("http://") || url.startsWith("https://")) {
            QLabel *lbl = new QLabel(QString("%1 (snapshot)").arg(name));
            lbl->setFixedSize(tileW, tileH);
            lbl->setAlignment(Qt::AlignCenter);
            lbl->setScaledContents(true);
            grid->addWidget(lbl, idx / 2, idx % 2);
            new HttpSnapshot(name, url, lbl, tileW, tileH, &win);
            idx++;
            continue;
        }

        QLabel *lbl = new QLabel(QString("%1 (RTSP)").arg(name));
        lbl->setFixedSize(tileW, tileH);
        lbl->setAlignment(Qt::AlignCenter);
        grid->addWidget(lbl, idx / 2, idx % 2);

        auto *w = new CameraWorker(name, url, lbl, tileW, tileH);
        w->start();
        workers.push_back(w);
        idx++;
        if (idx >= 4) break;
    }

    win.showFullScreen();
    int ret = app.exec();

    for (auto *w : workers) { w->requestInterruption(); w->wait(); delete w; }
    if (g_hw_device_ctx) av_buffer_unref(&g_hw_device_ctx);
    return ret;
}

#include "main.moc"
