
#include "selfdrive/ui/qt/onroad/annotated_camera.h"

#include <QPainter>
#include <algorithm>
#include <cmath>

#include "common/swaglog.h"
#include "selfdrive/ui/qt/util.h"
#include "selfdrive/ui/qt/onroad/buttons.h"

// Window that shows camera view and variety of info drawn on top
AnnotatedCameraWidget::AnnotatedCameraWidget(VisionStreamType type, QWidget *parent)
    : fps_filter(UI_FREQ, 3, 1. / UI_FREQ), CameraWidget("camerad", type, parent) {
  pm = std::make_unique<PubMaster>(std::vector<const char*>{"uiDebug"});

  main_layout = new QVBoxLayout(this);
  main_layout->setMargin(UI_BORDER_SIZE);
  main_layout->setSpacing(0);

  experimental_btn = new ExperimentalButton(this);
  main_layout->addWidget(experimental_btn, 0, Qt::AlignTop | Qt::AlignRight);

  // screen recoder - neokii
  record_timer = std::make_shared<QTimer>();
	QObject::connect(record_timer.get(), &QTimer::timeout, [=]() {
    if(recorder) {
      recorder->update_screen();
    }
  });
	record_timer->start(1000/UI_FREQ);

	recorder = new ScreenRecoder(this);
	main_layout->addWidget(recorder, 0, Qt::AlignBottom | Qt::AlignRight);
}

void AnnotatedCameraWidget::updateState(const UIState &s) {
  // update engageability/experimental mode button
  experimental_btn->updateState(s);
  dmon.updateState(s);
}

void AnnotatedCameraWidget::initializeGL() {
  CameraWidget::initializeGL();
  qInfo() << "OpenGL version:" << QString((const char*)glGetString(GL_VERSION));
  qInfo() << "OpenGL vendor:" << QString((const char*)glGetString(GL_VENDOR));
  qInfo() << "OpenGL renderer:" << QString((const char*)glGetString(GL_RENDERER));
  qInfo() << "OpenGL language version:" << QString((const char*)glGetString(GL_SHADING_LANGUAGE_VERSION));

  prev_draw_t = millis_since_boot();
  setBackgroundColor(bg_colors[STATUS_DISENGAGED]);
}

mat4 AnnotatedCameraWidget::calcFrameMatrix() {
  // Project point at "infinity" to compute x and y offsets
  // to ensure this ends up in the middle of the screen
  // for narrow come and a little lower for wide cam.
  // TODO: use proper perspective transform?

  // Select intrinsic matrix and calibration based on camera type
  auto *s = uiState();
  bool wide_cam = active_stream_type == VISION_STREAM_WIDE_ROAD;
  const auto &intrinsic_matrix = wide_cam ? ECAM_INTRINSIC_MATRIX : FCAM_INTRINSIC_MATRIX;
  const auto &calibration = wide_cam ? s->scene.view_from_wide_calib : s->scene.view_from_calib;

   // Compute the calibration transformation matrix
  const auto calib_transform = intrinsic_matrix * calibration;

  float zoom = wide_cam ? 2.0 : 1.1;
  Eigen::Vector3f inf(1000., 0., 0.);
  auto Kep = calib_transform * inf;

  int w = width(), h = height();
  float center_x = intrinsic_matrix(0, 2);
  float center_y = intrinsic_matrix(1, 2);

  float max_x_offset = center_x * zoom - w / 2 - 5;
  float max_y_offset = center_y * zoom - h / 2 - 5;
  float x_offset = std::clamp<float>((Kep.x() / Kep.z() - center_x) * zoom, -max_x_offset, max_x_offset);
  float y_offset = std::clamp<float>((Kep.y() / Kep.z() - center_y) * zoom, -max_y_offset, max_y_offset);

  // Apply transformation such that video pixel coordinates match video
  // 1) Put (0, 0) in the middle of the video
  // 2) Apply same scaling as video
  // 3) Put (0, 0) in top left corner of video
  Eigen::Matrix3f video_transform =(Eigen::Matrix3f() <<
    zoom, 0.0f, (w / 2 - x_offset) - (center_x * zoom),
    0.0f, zoom, (h / 2 - y_offset) - (center_y * zoom),
    0.0f, 0.0f, 1.0f).finished();

  model.setTransform(video_transform * calib_transform);

  float zx = zoom * 2 * center_x / w;
  float zy = zoom * 2 * center_y / h;
  return mat4{{
    zx, 0.0, 0.0, -x_offset / w * 2,
    0.0, zy, 0.0, y_offset / h * 2,
    0.0, 0.0, 1.0, 0.0,
    0.0, 0.0, 0.0, 1.0,
  }};
}

void AnnotatedCameraWidget::paintGL() {
}

void AnnotatedCameraWidget::paintEvent(QPaintEvent *event) {
  UIState *s = uiState();
  SubMaster &sm = *(s->sm);
  const double start_draw_t = millis_since_boot();

  QPainter p(this);

  // Wide or narrow cam dependent on speed
  bool has_wide_cam = available_streams.count(VISION_STREAM_WIDE_ROAD);
  if (has_wide_cam) {
    float v_ego = sm["carState"].getCarState().getVEgo();
    if ((v_ego < 10) || available_streams.size() == 1) {
      wide_cam_requested = true;
    } else if (v_ego > 15) {
      wide_cam_requested = false;
    }
    wide_cam_requested = wide_cam_requested && sm["selfdriveState"].getSelfdriveState().getExperimentalMode();
  }

  if(s->scene.show_driver_camera) {
    CameraWidget::setStreamType(VISION_STREAM_DRIVER);
  }
  else {
    CameraWidget::setStreamType(wide_cam_requested ? VISION_STREAM_WIDE_ROAD : VISION_STREAM_ROAD);
  }

  p.beginNativePainting();
  CameraWidget::setFrameId(sm["modelV2"].getModelV2().getFrameId());
  CameraWidget::paintGL();
  p.endNativePainting();

  if (!s->scene.show_driver_camera) {
    model.draw(p, rect());
    dmon.draw(p, rect());
    custom_hud.draw(p, rect());
  }

  double cur_draw_t = millis_since_boot();
  double dt = cur_draw_t - prev_draw_t;
  double fps = fps_filter.update(1. / dt * 1000);
  if (fps < 15) {
    LOGW("slow frame rate: %.2f fps", fps);
  }
  prev_draw_t = cur_draw_t;

  // publish debug msg
  MessageBuilder msg;
  auto m = msg.initEvent().initUiDebug();
  m.setDrawTimeMillis(cur_draw_t - start_draw_t);
  pm->send("uiDebug", msg);
}

void AnnotatedCameraWidget::showEvent(QShowEvent *event) {
  CameraWidget::showEvent(event);

  auto now = millis_since_boot();
  if(now - last_update_params > 1000*5) {
    last_update_params = now;
    ui_update_params(uiState());
  }

  prev_draw_t = millis_since_boot();
}