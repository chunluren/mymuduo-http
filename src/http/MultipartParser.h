/**
 * @file MultipartParser.h
 * @brief HTTP multipart/form-data 解析器
 *
 * 实现 RFC 2046 定义的 multipart 消息体解析，用于处理文件上传和
 * 混合表单提交。解析器为纯静态工具类，无状态，线程安全。
 *
 * 解析流程:
 * 1. 根据 boundary 分割各个 part
 * 2. 对每个 part 分离 headers 和 body（以 \\r\\n\\r\\n 为界）
 * 3. 从 Content-Disposition 头中提取 name 和 filename
 * 4. 从 Content-Type 头中提取该 part 的 MIME 类型
 *
 * @example 使用示例
 * @code
 * std::string boundary = MultipartParser::extractBoundary(contentTypeHeader);
 * auto parts = MultipartParser::parse(requestBody, boundary);
 * for (auto& part : parts) {
 *     if (part.isFile()) {
 *         saveFile(part.filename, part.data);
 *     }
 * }
 * @endcode
 */

#pragma once
#include <string>
#include <vector>
#include <unordered_map>

/**
 * @struct MultipartPart
 * @brief multipart 消息中的单个部分（表单字段或文件）
 */
struct MultipartPart {
    std::string name;                    ///< 表单字段名（Content-Disposition 中的 name 参数）
    std::string filename;                ///< 文件名（空字符串表示普通文本字段）
    std::string contentType;             ///< 该 part 的 MIME 类型（如 "image/png"）
    std::string data;                    ///< 正文数据（文本或二进制内容）

    /**
     * @brief 判断该 part 是否为文件上传
     * @return true 表示文件上传，false 表示普通文本字段
     */
    bool isFile() const { return !filename.empty(); }
};

/**
 * @class MultipartParser
 * @brief multipart/form-data 解析器（纯静态工具类）
 *
 * 提供两个核心静态方法:
 * - extractBoundary(): 从 Content-Type 头中提取 boundary 字符串
 * - parse(): 根据 boundary 解析 multipart 消息体为 MultipartPart 列表
 */
class MultipartParser {
public:
    /**
     * @brief 解析 multipart 消息体
     * @param body 原始 HTTP 请求体
     * @param boundary 分隔符字符串（从 Content-Type 头中提取）
     * @return 解析出的各个 part 的列表；若输入为空或格式错误返回空列表
     */
    static std::vector<MultipartPart> parse(const std::string& body, const std::string& boundary) {
        std::vector<MultipartPart> parts;
        if (body.empty() || boundary.empty()) {
            return parts;
        }

        /// multipart 消息格式中，每个 part 由 "--boundary" 分隔，
        /// 消息以 "--boundary--" 结束（RFC 2046）
        std::string delimiter = "--" + boundary;
        std::string endDelimiter = delimiter + "--";

        /// 定位第一个分隔符的位置
        size_t pos = body.find(delimiter);
        if (pos == std::string::npos) {
            return parts;
        }

        /// 跳过第一个分隔符行（delimiter + \r\n），定位到第一个 part 的起始位置
        pos = body.find("\r\n", pos);
        if (pos == std::string::npos) {
            return parts;
        }
        pos += 2; // 跳过 \r\n

        while (pos < body.size()) {
            /// 查找下一个分隔符，两个分隔符之间即为一个 part 的完整内容
            size_t nextDelim = body.find(delimiter, pos);
            if (nextDelim == std::string::npos) {
                break;
            }

            /// 提取当前 part 的内容（包含 headers + \r\n\r\n + body）
            std::string partContent = body.substr(pos, nextDelim - pos);

            /// 解析单个 part（分离 headers 和 body，提取 name/filename 等字段）
            MultipartPart part = parsePart(partContent);
            if (!part.name.empty()) {
                parts.push_back(std::move(part));
            }

            /// 移动到当前分隔符之后
            pos = nextDelim + delimiter.size();

            /// 检查是否为结束分隔符 "--boundary--"
            if (pos + 2 <= body.size() && body.substr(pos, 2) == "--") {
                break; // 所有 part 解析完毕
            }

            /// 跳过分隔符后的 \r\n，定位到下一个 part 的起始位置
            if (pos + 2 <= body.size() && body.substr(pos, 2) == "\r\n") {
                pos += 2;
            }
        }

        return parts;
    }

    /**
     * @brief 从 Content-Type 头值中提取 boundary 字符串
     * @param contentType Content-Type 头的值，如 "multipart/form-data; boundary=----xxx"
     * @return boundary 字符串；未找到时返回空字符串
     *
     * 支持带引号和不带引号两种格式:
     * - boundary="----WebKitFormBoundary"
     * - boundary=----WebKitFormBoundary
     */
    static std::string extractBoundary(const std::string& contentType) {
        /// 在 Content-Type 头中查找 "boundary=" 关键字
        std::string key = "boundary=";
        size_t pos = contentType.find(key);
        if (pos == std::string::npos) {
            return "";
        }
        pos += key.size();

        /// 处理带引号的 boundary 值: boundary="----WebKitFormBoundary..."
        if (pos < contentType.size() && contentType[pos] == '"') {
            pos++;
            size_t end = contentType.find('"', pos);
            if (end == std::string::npos) {
                return contentType.substr(pos);
            }
            return contentType.substr(pos, end - pos);
        }

        /// 处理不带引号的 boundary 值: boundary=----WebKitFormBoundary...
        /// 读取直到遇到空格、分号、换行符或字符串末尾
        size_t end = pos;
        while (end < contentType.size() && contentType[end] != ' ' &&
               contentType[end] != ';' && contentType[end] != '\r' && contentType[end] != '\n') {
            end++;
        }
        return contentType.substr(pos, end - pos);
    }

private:
    /**
     * @brief 解析单个 part 的内容（headers + body）
     * @param partContent 单个 part 的原始内容（从 boundary 之间截取）
     * @return 解析后的 MultipartPart；若格式错误则返回 name 为空的对象
     *
     * 解析步骤:
     * 1. 以 \\r\\n\\r\\n 分离 headers 和 body
     * 2. 去除 body 末尾的 \\r\\n（delimiter 前缀）
     * 3. 逐行解析 Content-Disposition 和 Content-Type 头
     */
    static MultipartPart parsePart(const std::string& partContent) {
        MultipartPart part;

        // Split headers from body at \r\n\r\n
        size_t headerEnd = partContent.find("\r\n\r\n");
        if (headerEnd == std::string::npos) {
            return part;
        }

        std::string headers = partContent.substr(0, headerEnd);
        std::string data = partContent.substr(headerEnd + 4);

        // Remove trailing \r\n from data (delimiter prefix)
        if (data.size() >= 2 && data.substr(data.size() - 2) == "\r\n") {
            data = data.substr(0, data.size() - 2);
        }
        part.data = std::move(data);

        // Parse headers line by line
        size_t pos = 0;
        while (pos < headers.size()) {
            size_t lineEnd = headers.find("\r\n", pos);
            std::string line;
            if (lineEnd == std::string::npos) {
                line = headers.substr(pos);
                pos = headers.size();
            } else {
                line = headers.substr(pos, lineEnd - pos);
                pos = lineEnd + 2;
            }

            // Parse Content-Disposition
            if (line.find("Content-Disposition:") != std::string::npos) {
                part.name = extractHeaderParam(line, "name");
                part.filename = extractHeaderParam(line, "filename");
            }
            // Parse Content-Type
            else if (line.find("Content-Type:") != std::string::npos) {
                size_t colonPos = line.find(':');
                if (colonPos != std::string::npos) {
                    std::string value = line.substr(colonPos + 1);
                    // Trim leading whitespace
                    size_t start = value.find_first_not_of(' ');
                    if (start != std::string::npos) {
                        part.contentType = value.substr(start);
                    }
                }
            }
        }

        return part;
    }

    /**
     * @brief 从头部行中提取带引号的参数值
     * @param line 头部行，如 "Content-Disposition: form-data; name=\"field1\"; filename=\"a.txt\""
     * @param param 参数名，如 "name" 或 "filename"
     * @return 参数值（去除引号后）；未找到时返回空字符串
     *
     * 例: extractHeaderParam("...name=\"field1\"...", "name") -> "field1"
     */
    static std::string extractHeaderParam(const std::string& line, const std::string& param) {
        std::string key = param + "=\"";
        size_t pos = line.find(key);
        if (pos == std::string::npos) {
            return "";
        }
        pos += key.size();
        size_t end = line.find('"', pos);
        if (end == std::string::npos) {
            return "";
        }
        return line.substr(pos, end - pos);
    }
};
