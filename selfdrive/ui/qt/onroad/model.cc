#include "selfdrive/ui/qt/onroad/model.h"

#include <QString>
#include "selfdrive/ui/qt/util.h"

constexpr int CLIP_MARGIN = 500;
constexpr float MIN_DRAW_DISTANCE = 10.0;
constexpr float MAX_DRAW_DISTANCE = 100.0;

static int get_path_length_idx(const cereal::XYZTData::Reader &line, const float path_height) {
  const auto &line_x = line.getX();
  int max_idx = 0;
  for (int i = 1; i < line_x.size() && line_x[i] <= path_height; ++i) {
    max_idx = i;
  }
  return max_idx;
}

void ModelRenderer::updateState(const UIState &s) {
  const UIScene &scene = s.scene;
  const SubMaster &sm = *(s.sm);
  const auto ce = sm["carState"].getCarState();
  const bool cs_alive = sm.alive("carState");

  // Handle older routes where vEgoCluster is not set
  v_ego_cluster_seen = v_ego_cluster_seen || ce.getVEgoCluster() != 0.0;
  float v_ego = v_ego_cluster_seen ? ce.getVEgoCluster() : ce.getVEgo();
  speed = cs_alive ? std::max<float>(0.0f, v_ego * (scene.is_metric ? MS_TO_KPH : MS_TO_MPH)) : 0.0;

  left_blindspot = ce.getLeftBlindspot();
  right_blindspot = ce.getRightBlindspot();
}

void ModelRenderer::drawTextColor(QPainter &p, int x, int y, const QString &text, const QColor &color) {
  p.setOpacity(1.0);
  QRect real_rect = p.fontMetrics().boundingRect(text);
  real_rect.moveCenter({x, y - real_rect.height() / 2});
  p.setPen(color);
  p.drawText(real_rect.x(), real_rect.bottom(), text);
}

void ModelRenderer::draw(QPainter &painter, const QRect &surface_rect) {
  auto &sm = *(uiState()->sm);
  if (sm.updated("carParams")) {
    longitudinal_control = sm["carParams"].getCarParams().getOpenpilotLongitudinalControl();
  }

  // Check if data is up-to-date
  if (!(sm.alive("liveCalibration") && sm.alive("modelV2"))) {
    return;
  }

  clip_region = surface_rect.adjusted(-CLIP_MARGIN, -CLIP_MARGIN, CLIP_MARGIN, CLIP_MARGIN);
  experimental_model = sm["selfdriveState"].getSelfdriveState().getExperimentalMode();

  painter.save();

  const auto &model = sm["modelV2"].getModelV2();
  const auto &radar_state = sm["radarState"].getRadarState();
  const auto &lead_one = radar_state.getLeadOne();

  update_model(model, lead_one);
  drawLaneLines(painter);
  drawPath(painter, model, surface_rect.height());

  //if (longitudinal_control && sm.alive("radarState")) {
  if (sm.alive("radarState")) {
    update_leads(radar_state, model.getPosition());
    const auto &lead_two = radar_state.getLeadTwo();
    if (lead_one.getStatus()) {
      drawLead(painter, lead_one, lead_vertices[0], surface_rect);
    }
    if (lead_two.getStatus() && (std::abs(lead_one.getDRel() - lead_two.getDRel()) > 3.0)) {
      drawLead(painter, lead_two, lead_vertices[1], surface_rect);
    }
  }

  painter.restore();
}

void ModelRenderer::update_leads(const cereal::RadarState::Reader &radar_state, const cereal::XYZTData::Reader &line) {
  for (int i = 0; i < 2; ++i) {
    const auto &lead_data = (i == 0) ? radar_state.getLeadOne() : radar_state.getLeadTwo();
    if (lead_data.getStatus()) {
      float z = line.getZ()[get_path_length_idx(line, lead_data.getDRel())];
      mapToScreen(lead_data.getDRel(), -lead_data.getYRel(), z + 1.22, &lead_vertices[i]);
    }
  }
}

void ModelRenderer::update_model(const cereal::ModelDataV2::Reader &model, const cereal::RadarState::LeadData::Reader &lead) {
  const auto &model_position = model.getPosition();
  float max_distance = std::clamp(*(model_position.getX().end() - 1), MIN_DRAW_DISTANCE, MAX_DRAW_DISTANCE);

  // update lane lines
  const auto &lane_lines = model.getLaneLines();
  const auto &line_probs = model.getLaneLineProbs();
  int max_idx = get_path_length_idx(lane_lines[0], max_distance);
  for (int i = 0; i < std::size(lane_line_vertices); i++) {
    lane_line_probs[i] = line_probs[i];
    mapLineToPolygon(lane_lines[i], 0.025 * lane_line_probs[i], 0, 0, &lane_line_vertices[i], max_idx);
  }

  // update road edges
  const auto &road_edges = model.getRoadEdges();
  const auto &edge_stds = model.getRoadEdgeStds();
  for (int i = 0; i < std::size(road_edge_vertices); i++) {
    road_edge_stds[i] = edge_stds[i];
    mapLineToPolygon(road_edges[i], 0.025, 0, 0, &road_edge_vertices[i], max_idx);
  }

  // update path
  if (lead.getStatus()) {
    const float lead_d = lead.getDRel() * 2.;
    max_distance = std::clamp((float)(lead_d - fmin(lead_d * 0.35, 10.)), 0.0f, max_distance);
  }
  max_idx = get_path_length_idx(model_position, max_distance);
  mapLineToPolygon(model_position, 0.8, 1.22, 1.22, &track_vertices, max_idx, false);
}

void ModelRenderer::drawLaneLines(QPainter &painter) {
  // lanelines
  for (int i = 0; i < std::size(lane_line_vertices); ++i) {
    painter.setBrush(QColor::fromRgbF(1.0, 1.0, 1.0, std::clamp<float>(lane_line_probs[i], 0.0, 0.7)));
    painter.drawPolygon(lane_line_vertices[i]);
  }

  // TODO: Fix empty spaces when curiving back on itself
  painter.setBrush(QColor::fromRgbF(1.0, 0.0, 0.0, 0.2));
  if (left_blindspot) painter.drawPolygon(lane_barrier_vertices[0]);
  if (right_blindspot) painter.drawPolygon(lane_barrier_vertices[1]);

  // road edges
  for (int i = 0; i < std::size(road_edge_vertices); ++i) {
    painter.setBrush(QColor::fromRgbF(1.0, 0, 0, std::clamp<float>(1.0 - road_edge_stds[i], 0.0, 1.0)));
    painter.drawPolygon(road_edge_vertices[i]);
  }
}

void ModelRenderer::drawPath(QPainter &painter, const cereal::ModelDataV2::Reader &model, int height) {
  QLinearGradient bg(0, height, 0, 0);
  UIState *s = uiState();

  //if (experimental_model) {
  if (s->scene.engaged) {
    if (s->scene.steeringPressed) {
      // The user is applying torque to the steering wheel
      bg.setColorAt(0.0, steeringpressedColor(100));
      bg.setColorAt(0.5, steeringpressedColor(50));
      bg.setColorAt(1.0, steeringpressedColor(0));
    } else {
      // The first half of track_vertices are the points for the right side of the path
      const auto &acceleration = model.getAcceleration().getX();
      const int max_len = std::min<int>(track_vertices.length() / 2, acceleration.size());

      for (int i = 0; i < max_len; ++i) {
        // Some points are out of frame
        int track_idx = max_len - i - 1;  // flip idx to start from bottom right
        if (track_vertices[track_idx].y() < 0 || track_vertices[track_idx].y() > height) continue;

        // Flip so 0 is bottom of frame
        float lin_grad_point = (height - track_vertices[track_idx].y()) / height;

        // speed up: 120, slow down: 0
        float path_hue = fmax(fmin(60 + acceleration[i] * 35, 120), 0);
        // FIXME: painter.drawPolygon can be slow if hue is not rounded
        path_hue = int(path_hue * 100 + 0.5) / 100;

        float saturation = fmin(fabs(acceleration[i] * 1.5), 1);
        float lightness = util::map_val(saturation, 0.0f, 1.0f, 0.95f, 0.62f);        // lighter when grey
        float alpha = util::map_val(lin_grad_point, 0.75f / 2.f, 0.75f, 0.4f, 0.0f);  // matches previous alpha fade
        bg.setColorAt(lin_grad_point, QColor::fromHslF(path_hue / 360., saturation, lightness, alpha));

        // Skip a point, unless next is last
        i += (i + 2) < max_len ? 1 : 0;
      }
   }
  } else {
    //bg.setColorAt(0.0, QColor::fromHslF(148 / 360., 0.94, 0.51, 0.4));
    //bg.setColorAt(0.5, QColor::fromHslF(112 / 360., 1.0, 0.68, 0.35));
    //bg.setColorAt(1.0, QColor::fromHslF(112 / 360., 1.0, 0.68, 0.0));
    bg.setColorAt(0.0, whiteColor(100));
    bg.setColorAt(0.5, whiteColor(50));
    bg.setColorAt(1.0, whiteColor(0));
  }

  painter.setBrush(bg);
  painter.drawPolygon(track_vertices);
}

void ModelRenderer::drawLead(QPainter &painter, const cereal::RadarState::LeadData::Reader &lead_data,
                             const QPointF &vd, const QRect &surface_rect) {
  const float speedBuff = 10.;
  const float leadBuff = 40.;
  const float d_rel = lead_data.getDRel();
  const float v_rel = lead_data.getVRel();
  UIState *s = uiState();

  float fillAlpha = 0;
  if (d_rel < leadBuff) {
    fillAlpha = 255 * (1.0 - (d_rel / leadBuff));
    if (v_rel < 0) {
      fillAlpha += 255 * (-1 * (v_rel / speedBuff));
    }
    fillAlpha = (int)(fmin(fillAlpha, 255));
  }

  float sz = std::clamp((25 * 30) / (d_rel / 3 + 30), 15.0f, 30.0f) * 2.35;
  float x = std::clamp<float>(vd.x(), 0.f, surface_rect.width() - sz / 2);
  float y = std::min<float>(vd.y(), surface_rect.height() - sz * 0.6);

  float g_xo = sz / 5;
  float g_yo = sz / 10;

  QPointF glow[] = {{x + (sz * 1.35) + g_xo, y + sz + g_yo}, {x, y - g_yo}, {x - (sz * 1.35) - g_xo, y + sz + g_yo}};
  painter.setBrush(pinkColor());
  painter.drawPolygon(glow, std::size(glow));

  // chevron
  QPointF chevron[] = {{x + (sz * 1.25), y + sz}, {x, y}, {x - (sz * 1.25), y + sz}};
  painter.setBrush(redColor(fillAlpha));
  painter.drawPolygon(chevron, std::size(chevron));

  // lead car radar distance and speed
  QString l_dist, l_speed;
  QColor d_color, v_color = whiteColor(150);

  if (d_rel < 5) {
    d_color = redColor(150);
  } else if (d_rel < 15) {
    d_color = orangeColor(150);
  } else {
    d_color = whiteColor(150);
  }
  l_dist = QString::asprintf("%.1f m", d_rel);

  if (v_rel < -4.4704) {
    v_color = redColor(150);
  } else if (v_rel < 0) {
    v_color = orangeColor(150);
  } else {
    v_color = pinkColor(150);
  }
  if (s->scene.is_metric) {
    l_speed = QString::asprintf("%.0f km/h", speed + v_rel * 3.6);
  } else {
    l_speed = QString::asprintf("%.0f mph", speed + v_rel * 2.236936);
  }
  painter.setFont(InterFont(35, QFont::Bold));
  drawTextColor(painter, x, y + sz / 1.5f + 70.0, l_dist, d_color);
  drawTextColor(painter, x, y + sz / 1.5f + 120.0, l_speed, v_color);
}

// Projects a point in car to space to the corresponding point in full frame image space.
bool ModelRenderer::mapToScreen(float in_x, float in_y, float in_z, QPointF *out) {
  Eigen::Vector3f input(in_x, in_y, in_z);
  auto pt = car_space_transform * input;
  *out = QPointF(pt.x() / pt.z(), pt.y() / pt.z());
  return clip_region.contains(*out);
}

void ModelRenderer::mapLineToPolygon(const cereal::XYZTData::Reader &line, float y_off, float z_off_left, float z_off_right,
                                     QPolygonF *pvd, int max_idx, bool allow_invert) {
  const auto line_x = line.getX(), line_y = line.getY(), line_z = line.getZ();
  QPointF left, right;
  pvd->clear();
  for (int i = 0; i <= max_idx; i++) {
    // highly negative x positions  are drawn above the frame and cause flickering, clip to zy plane of camera
    if (line_x[i] < 0) continue;

    bool l = mapToScreen(line_x[i], line_y[i] - y_off, line_z[i] + z_off_left, &left);
    bool r = mapToScreen(line_x[i], line_y[i] + y_off, line_z[i] + z_off_right, &right);
    if (l && r) {
      // For wider lines the drawn polygon will "invert" when going over a hill and cause artifacts
      if (!allow_invert && pvd->size() && left.y() > pvd->back().y()) {
        continue;
      }
      pvd->push_back(left);
      pvd->push_front(right);
    }
  }
}
