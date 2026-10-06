// SPDX-License-Identifier: MPL-2.0
#include <alpakaMetrics/Json.hpp>

#include <cmath>
#include <concepts>
#include <iomanip>
#include <limits>
#include <locale>
#include <sstream>

namespace alpakaMetrics
{
    namespace
    {
        void quoted(std::ostream& out, std::string_view text)
        {
            constexpr char hex[] = "0123456789abcdef";
            out << '"';
            for(unsigned char character : text)
            {
                if(character == '"' || character == '\\')
                    out << '\\' << static_cast<char>(character);
                else if(character < 0x20)
                    out << "\\u00" << hex[character >> 4] << hex[character & 0xf];
                else
                    out << static_cast<char>(character);
            }
            out << '"';
        }

        template<std::size_t N>
        std::string_view enumName(auto value, std::string_view const (&names)[N])
        {
            auto const index = static_cast<std::size_t>(value);
            if(index >= N)
                throw std::invalid_argument{"Invalid enum value in JSON result"};
            return names[index];
        }

        constexpr std::string_view units[]
            = {"seconds", "count", "joules", "watts", "hertz", "ratio", "bytes", "providerDefined"};
        constexpr std::string_view scopes[]
            = {"hostRegion",
               "callingThread",
               "queueWorkerThread",
               "queueInterval",
               "device",
               "context",
               "providerDefined"};
        constexpr std::string_view statuses[]
            = {"available",
               "unsupported",
               "dependencyDisabled",
               "permissionDenied",
               "conflicting",
               "unsupportedScope",
               "collectionFailed"};
        constexpr std::string_view operations[] = {"hostRegion", "kernel", "hostTask"};
    } // namespace

    std::string toJson(Result const& result)
    {
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<double>::max_digits10) << std::boolalpha;
        auto field = [&](std::string_view name, std::string_view value)
        {
            quoted(out, name);
            out << ':';
            quoted(out, value);
        };
        out << "{\"schemaVersion\":1,\"measurementId\":" << result.measurementId << ',';
        field("label", result.label);
        out << ",\"synchronized\":" << result.synchronized << ",\"replayed\":" << result.replayed
            << ",\"passCount\":" << result.passCount
            << ",\"provenance\":{\"sessionId\":" << result.provenance.sessionId
            << ",\"queueId\":" << result.provenance.queueId << ',';
        field("kind", enumName(result.provenance.kind, operations));
        out << ',';
        field("queueName", result.provenance.queueName);
        out << ',';
        field("deviceName", result.provenance.deviceName);
        out << ',';
        field("api", result.provenance.api);
        out << "},\"metrics\":[";
        bool first = true;
        for(auto const& metric : result.metrics)
        {
            if(!first)
                out << ',';
            first = false;
            auto const& d = metric.descriptor;
            out << '{';
            field("name", d.name);
            out << ',';
            field("nativeName", d.nativeName);
            out << ',';
            field("description", d.description);
            out << ',';
            field("provider", d.provider);
            out << ',';
            field("collector", d.collector);
            out << ',';
            field("collectorVersion", d.collectorVersion);
            out << ',';
            field("collectorPath", d.collectorPath);
            out << ',';
            field("unit", enumName(d.unit, units));
            out << ',';
            field("scope", enumName(d.scope, scopes));
            out << ',';
            field("nativeUnit", d.nativeUnit);
            if(!std::isfinite(d.nativeToValueScale))
                throw std::invalid_argument{"Non-finite scale cannot be serialized to JSON"};
            out << ",\"nativeToValueScale\":" << d.nativeToValueScale << ',';
            field("status", enumName(metric.status, statuses));
            out << ',';
            field("diagnostic", metric.diagnostic);
            out << ",\"valueType\":";
            if(!metric.value)
                out << "null,\"value\":null";
            else
            {
                constexpr std::string_view types[] = {"int64", "uint64", "double"};
                quoted(out, types[metric.value->index()]);
                out << ",\"value\":";
                std::visit(
                    [&](auto value)
                    {
                        if constexpr(std::same_as<decltype(value), double>)
                            if(!std::isfinite(value))
                                throw std::invalid_argument{"Non-finite metric cannot be serialized to JSON"};
                        out << value;
                    },
                    *metric.value);
            }
            out << '}';
        }
        out << "]}";
        return out.str();
    }
} // namespace alpakaMetrics
