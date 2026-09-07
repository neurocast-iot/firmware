/**
 * @file file_upload_service_stub.cpp
 * @brief FileUploadService::enqueue 桩实现
 *
 * x86 测试环境不编 file_upload_service.cpp（它依赖 HTTP 客户端、worker 线程等），
 * 但 rpc_handler.cpp 里 handleUploadFile 调了 enqueue()，链接器需要这个符号。
 * 测试里 uploadService 传的是 nullptr，不会真正调到这个方法。
 */
#include "upload/file_upload_service.h"

namespace iot_agent {

bool FileUploadService::enqueue(const UploadTask&) {
    return false;  /* 桩：测试里不调用真实上传 */
}

} // namespace iot_agent
