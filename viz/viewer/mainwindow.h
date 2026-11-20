/*
 * Copyright (C) 2026 by Thun Lu. All rights reserved.
 * Author: Thun Lu <thun.lu@zohomail.cn>
 * Repo:   https://github.com/thun-res/vlink
 *  _    __   __      _           __
 * | |  / /  / /     (_) ____    / /__
 * | | / /  / /     / / / __ \  / //_/
 * | |/ /  / /___  / / / / / / / ,<
 * |___/  /_____/ /_/ /_/ /_/ /_/|_|
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

// NOLINTBEGIN

#pragma once

#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wcpp"
#endif
#include <google/protobuf/compiler/importer.h>
#include <google/protobuf/descriptor.h>
#include <google/protobuf/dynamic_message.h>
#include <google/protobuf/message.h>
#include <google/protobuf/reflection.h>
#if defined(__GNUC__) && !defined(__clang__)
#pragma GCC diagnostic pop
#endif

#include "./flatbuffers_runtime_compat.h"

//
#include <vlink/base/elapsed_timer.h>
#include <vlink/extension/bag_reader.h>
#include <vlink/external/proxy_api.h>
#include <vlink/vlink.h>

#include <QCloseEvent>
#include <QElapsedTimer>
#include <QLabel>
#include <QMainWindow>
#include <QProcess>
#include <QResizeEvent>
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QSqlQueryModel>
#include <QTimer>
#include <QTreeWidgetItem>
#include <map>
#include <memory>
#include <shared_mutex>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

Q_DECLARE_METATYPE(vlink::ProxyAPI::Data)
Q_DECLARE_METATYPE(std::vector<vlink::ProxyAPI::Info>)

QT_BEGIN_NAMESPACE
namespace Ui {
class MainWindow;
}
QT_END_NAMESPACE

class MainWindow : public QMainWindow {
  Q_OBJECT

 public:
  explicit MainWindow(QWidget* parent = nullptr);

  ~MainWindow();

  static void create_instance();

  static void destroy_instance();

  static MainWindow* get_instance();

  std::vector<QString> scanned_message_types() const;

  static void open_url(const QString& url);

 private slots:
  void on_actionLocal_D_triggered(bool checked);

  void on_actionAnalyzer_K_triggered();

  void on_actionTopology_N_triggered();

  void on_actionRecord_R_triggered();

  void on_actionPlay_P_triggered();

  void on_actionEdit_E_triggered();

  void on_actionRaw_J_triggered();

  void on_actionCamera_S_triggered();

  void on_actionPoint3D_Z_triggered();

  void on_actionPerception_triggered();

  void on_actionMap_G_triggered();

  void on_actionQuit_Q_triggered();

  void on_actionAboutQt_A_triggered();

  void on_actionAbout_this_S_triggered();

  void on_actionHow_to_use_U_triggered();

  void on_actionBug_Report_B_triggered();

  void on_actionDownload_L_triggered();

  void on_actionDB_Browser_W_triggered();

  void on_actionProtobuf_Decoder_F_triggered();

  void on_actionCommunication_Matrix_M_triggered();

  void on_actionStatus_Viewer_triggered(bool checked);

  void on_actionProto_Viewer_triggered(bool checked);

  void on_pushButton_protoselect_clicked();

  void on_pushButton_protoreload_clicked();

  void on_pushButton_fbsselect_clicked();

  void on_pushButton_fbsreload_clicked();

  void on_checkBox_showfreq_clicked(bool checked);

  void on_checkBox_showrate_clicked(bool checked);

  void on_checkBox_showloss_clicked(bool checked);

  void on_checkBox_showlantency_clicked(bool checked);

  void on_checkBox_view_clicked(bool checked);

  void on_checkBox_perf_clicked(bool checked);

  void on_checkBox_array_clicked(bool checked);

  void on_checkBox_hex_clicked(bool checked);

  void on_checkBox_enum_clicked(bool checked);

  void on_checkBox_time_clicked(bool checked);

  void on_pushButton_analyze_clicked();

  void on_pushButton_dataselect_clicked();

  void on_pushButton_datareload_clicked();

  void on_pushButton_datadetails_clicked();

  void on_pushButton_jump_clicked();

  void update_connected(bool connected);

  void update_error(int error);

  void update_time();

  void update_url_widget(const QVariant& variant);

  void update_proto_ser();

  void update_property_widget(const QVariant& variant, const QElapsedTimer& timer, bool refresh);

  void update_process_widget();

  void adjust_size();

  vlink::SchemaData search_schema(const std::string& name, vlink::SchemaType schema_type = vlink::SchemaType::kUnknown);

 private:
  void process_current_item_changed();

  void reload_root_msg(QTreeWidgetItem* item);

  bool update_proto_root_msg(const std::string& url, const std::string& ser, bool force);

  void update_local_proto();

  QString get_str_for_number(int64_t num);

  QString get_str_for_unsigned_number(uint64_t num);

  QString get_str_for_enum(const std::string& enum_str, int64_t num);

  int get_int_for_str(const QString& str);

  qlonglong get_longlong_for_str(const QString& str);

  unsigned int get_uint_for_str(const QString& str);

  qulonglong get_ulonglong_for_str(const QString& str);

  void import_protos(const QString& dir, bool& has_import, int depth = 0);

  void send_control(vlink::ProxyAPI::Mode mode, bool has_url = true);

  bool select_source_dir(const std::string& dir);

  bool select_fbs_dir(const std::string& dir);

  bool get_property_list(class QTreeWidget* widget, const std::string& parent_id, const google::protobuf::Message* msg);

  bool set_property_list(class QTreeWidget* widget, const std::string& parent_id, google::protobuf::Message* msg);

  bool get_flatbuffers_property_list(class QTreeWidget* widget, const std::string& parent_id,
                                     const FlatbuffersObjectView& view, const FlatbuffersSchemaContext& ctx);

  QTreeWidgetItem* get_item_property(class QTreeWidget* widget, const std::string& parent_id, const std::string& id);

  void clear_all_url_item();

  void clear_all_property_item(class QTreeWidget* widget);

  void clear_all_process_item();

  void remove_all_item(QTreeWidgetItem* item);

  void message_box_todo(class QWidget* parent);

  void update_status_bar(int total, int active, int select, int64_t rate, double loss);

  void check_new_version();

  void update_zero_copy_item_property(const vlink::Bytes& bytes);

 protected:
  void showEvent(class QShowEvent* event) override;

  void hideEvent(class QHideEvent* event) override;

  void closeEvent(class QCloseEvent* event) override;

  void resizeEvent(class QResizeEvent* event) override;

 private:
  inline static std::atomic<MainWindow*> instance_{nullptr};
  Ui::MainWindow* ui{nullptr};
  class AnalyzeDialog* analyze_dialog_{nullptr};
  QLabel* status_label1_{nullptr};
  QLabel* status_label2_{nullptr};
  QTimer* sys_timer_{nullptr};
  QTimer* flag_timer_{nullptr};
  QTimer* filter_timer_{nullptr};
  QSqlDatabase local_database_;
  vlink::BagReader::Info local_info_;
  std::unordered_map<std::string, vlink::SchemaType> local_schema_type_map_;
  bool local_use_compress_{false};
  vlink::ElapsedTimer info_elapsed_timer_;

  std::mutex schema_mtx_;
  std::unordered_map<std::string, vlink::SchemaData> schema_map_;

  std::shared_ptr<vlink::ProxyAPI> proxy_;

  std::shared_ptr<google::protobuf::compiler::DiskSourceTree> source_tree_;
  std::shared_ptr<google::protobuf::compiler::Importer> importer_;
  std::shared_ptr<google::protobuf::DynamicMessageFactory> factory_;

  google::protobuf::DescriptorPool* des_pool_{nullptr};
  google::protobuf::FileDescriptor* file_desc_{nullptr};
  std::vector<const google::protobuf::FileDescriptor*> proto_files_;
  google::protobuf::Descriptor* desc_{nullptr};
  google::protobuf::Message* root_msg_{nullptr};
  bool is_zero_copy_types_{false};
  bool is_flatbuffers_types_{false};
  bool is_in_model_{false};

  std::atomic_bool is_show_warn_{false};

  std::string current_url_;
  std::string current_ser_;
  std::string source_dir_;
  std::string fbs_dir_;
  FlatbuffersRuntime flatbuffers_runtime_;
  std::shared_ptr<FlatbuffersSchemaContext> current_fbs_context_;

  std::unordered_map<std::string, QTreeWidgetItem*> id_to_item_map_;
  std::unordered_map<QTreeWidgetItem*, google::protobuf::Message*> item_to_msg_map_;
  std::unordered_map<QTreeWidgetItem*, google::protobuf::FieldDescriptor*> item_to_field_map_;
  std::unordered_set<QTreeWidgetItem*> all_item_list_;
  std::unordered_set<QTreeWidgetItem*> to_hide_item_list_;
  std::unordered_map<std::string, vlink::Bytes> last_data_map_;
  std::unordered_map<std::string, std::vector<vlink::ProxyAPI::Process>> process_map_;
  std::unordered_map<std::string, std::string> ser_map_;
  std::unordered_map<std::string, vlink::SchemaType> schema_type_map_;

  std::vector<std::string> can_record_urls_;

  std::atomic<vlink::ProxyAPI::Mode> current_proxy_mode_{vlink::ProxyAPI::kOffline};

  vlink::SampleLostInfo last_sample_info_;
  std::atomic<int64_t> total_data_seq_{0};
  std::atomic<int64_t> total_data_latency_{0};

  QList<QTreeWidgetItem*> last_select_items_;

  vlink::ProxyAPI::DataCallback data_callback_;
  vlink::ProxyAPI::DataCallback data_callback2_;
  vlink::ProxyAPI::InfoCallback info_callback_;
  std::shared_mutex data_mutex_;

  std::unordered_map<std::string, QTreeWidgetItem*> zerocopy_item_map_;

  QProcess analyzer_process_;

  class QNetworkAccessManager* network_manager_{nullptr};

  QElapsedTimer property_timer_;

  class QRegularExpressionValidator* validator_int32_{nullptr};
  class QRegularExpressionValidator* validator_int64_{nullptr};
  class QRegularExpressionValidator* validator_uint32_{nullptr};
  class QRegularExpressionValidator* validator_uint64_{nullptr};
  class QRegularExpressionValidator* validator_double_{nullptr};
  class QRegularExpressionValidator* validator_float_{nullptr};
  class QRegularExpressionValidator* validator_bool_{nullptr};
  class QRegularExpressionValidator* validator_enum_{nullptr};
  class QRegularExpressionValidator* validator_string_{nullptr};

  class QRegularExpressionValidator* validator_normal_int_{nullptr};

  friend class TopologyDialog;
  friend class AnalyzeDialog;
  friend class RecordDialog;
  friend class PlayDialog;
  friend class EditDialog;
  friend class RawDialog;
  friend class CameraDialog;
  friend class Point3DDialog;
  friend class PerceptionDialog;
  friend class PerceptionEditorDialog;
  friend class AboutDialog;
  friend class SettingDialog;
  friend class EditItemDelegate;

 signals:
  void connect_changed(bool connected);
};

// NOLINTEND
