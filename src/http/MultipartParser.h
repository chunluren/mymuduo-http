#pragma once
#include <string>
#include <vector>
#include <unordered_map>

struct MultipartPart {
    std::string name;                    // form field name
    std::string filename;                // filename (empty for text fields)
    std::string contentType;             // content type of this part
    std::string data;                    // body data (text or binary)

    bool isFile() const { return !filename.empty(); }
};

class MultipartParser {
public:
    /// Parse multipart body
    /// @param body Raw request body
    /// @param boundary Boundary string (from Content-Type header)
    /// @return Vector of parsed parts
    static std::vector<MultipartPart> parse(const std::string& body, const std::string& boundary) {
        std::vector<MultipartPart> parts;
        if (body.empty() || boundary.empty()) {
            return parts;
        }

        std::string delimiter = "--" + boundary;
        std::string endDelimiter = delimiter + "--";

        // Find first delimiter
        size_t pos = body.find(delimiter);
        if (pos == std::string::npos) {
            return parts;
        }

        // Skip past first delimiter line
        pos = body.find("\r\n", pos);
        if (pos == std::string::npos) {
            return parts;
        }
        pos += 2; // skip \r\n

        while (pos < body.size()) {
            // Find end of this part (next delimiter)
            size_t nextDelim = body.find(delimiter, pos);
            if (nextDelim == std::string::npos) {
                break;
            }

            // Extract this part's content (headers + body)
            std::string partContent = body.substr(pos, nextDelim - pos);

            // Parse the part
            MultipartPart part = parsePart(partContent);
            if (!part.name.empty()) {
                parts.push_back(std::move(part));
            }

            // Move past the delimiter
            pos = nextDelim + delimiter.size();

            // Check if this is the end delimiter
            if (pos + 2 <= body.size() && body.substr(pos, 2) == "--") {
                break; // end of multipart
            }

            // Skip \r\n after delimiter
            if (pos + 2 <= body.size() && body.substr(pos, 2) == "\r\n") {
                pos += 2;
            }
        }

        return parts;
    }

    /// Extract boundary from Content-Type header value
    /// e.g. "multipart/form-data; boundary=----xxx" -> "----xxx"
    static std::string extractBoundary(const std::string& contentType) {
        std::string key = "boundary=";
        size_t pos = contentType.find(key);
        if (pos == std::string::npos) {
            return "";
        }
        pos += key.size();

        // Boundary may be quoted
        if (pos < contentType.size() && contentType[pos] == '"') {
            pos++;
            size_t end = contentType.find('"', pos);
            if (end == std::string::npos) {
                return contentType.substr(pos);
            }
            return contentType.substr(pos, end - pos);
        }

        // Unquoted: read until whitespace or semicolon or end
        size_t end = pos;
        while (end < contentType.size() && contentType[end] != ' ' &&
               contentType[end] != ';' && contentType[end] != '\r' && contentType[end] != '\n') {
            end++;
        }
        return contentType.substr(pos, end - pos);
    }

private:
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

    /// Extract a quoted parameter value from a header line
    /// e.g. extractHeaderParam("...name=\"field1\"...", "name") -> "field1"
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
