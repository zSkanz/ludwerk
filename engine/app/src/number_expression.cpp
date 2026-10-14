#include "engine/app/number_expression.h"

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <string>

namespace engine::app {
namespace {

// A recursive descent over the text: a sum of products of signed numbers and
// parenthesised sums.
class NumberExpression
{
public:
    explicit NumberExpression(std::string_view text) : m_text(text) {}

    [[nodiscard]] std::optional<double> evaluate()
    {
        const std::optional<double> value = sum();
        skipBlanks();
        if (!value.has_value() || m_at != m_text.size() || !std::isfinite(*value))
            return std::nullopt;
        return value;
    }

private:
    void skipBlanks()
    {
        while (m_at < m_text.size() && (m_text[m_at] == ' ' || m_text[m_at] == '\t'))
            ++m_at;
    }

    // A unit after a number: letters, and the degree sign's two bytes.
    void skipUnit()
    {
        skipBlanks();
        while (m_at < m_text.size()) {
            const auto c = static_cast<unsigned char>(m_text[m_at]);
            if (std::isalpha(c) != 0 || c == 0xC2 || c == 0xB0)
                ++m_at;
            else
                break;
        }
    }

    [[nodiscard]] bool take(char c)
    {
        skipBlanks();
        if (m_at < m_text.size() && m_text[m_at] == c) {
            ++m_at;
            return true;
        }
        return false;
    }

    [[nodiscard]] std::optional<double> sum()
    {
        std::optional<double> left = product();
        while (left.has_value()) {
            if (take('+')) {
                const std::optional<double> right = product();
                left = right.has_value() ? std::optional<double>(*left + *right) : std::nullopt;
            }
            else if (take('-')) {
                const std::optional<double> right = product();
                left = right.has_value() ? std::optional<double>(*left - *right) : std::nullopt;
            }
            else {
                break;
            }
        }
        return left;
    }

    [[nodiscard]] std::optional<double> product()
    {
        std::optional<double> left = unary();
        while (left.has_value()) {
            if (take('*')) {
                const std::optional<double> right = unary();
                left = right.has_value() ? std::optional<double>(*left * *right) : std::nullopt;
            }
            else if (take('/')) {
                const std::optional<double> right = unary();
                if (!right.has_value() || *right == 0.0)
                    return std::nullopt;
                left = *left / *right;
            }
            else {
                break;
            }
        }
        return left;
    }

    [[nodiscard]] std::optional<double> unary()
    {
        if (take('-')) {
            const std::optional<double> inner = unary();
            return inner.has_value() ? std::optional<double>(-*inner) : std::nullopt;
        }
        if (take('+'))
            return unary();
        if (take('(')) {
            const std::optional<double> inner = sum();
            if (!inner.has_value() || !take(')'))
                return std::nullopt;
            skipUnit();
            return inner;
        }
        return number();
    }

    [[nodiscard]] std::optional<double> number()
    {
        skipBlanks();
        std::string digits;
        bool seenDigit = false;
        bool seenPoint = false;
        while (m_at < m_text.size()) {
            const char c = m_text[m_at];
            if (c >= '0' && c <= '9') {
                digits.push_back(c);
                seenDigit = true;
            }
            else if ((c == '.' || c == ',') && !seenPoint) {
                digits.push_back('.');
                seenPoint = true;
            }
            else if ((c == 'e' || c == 'E') && seenDigit && m_at + 1 < m_text.size() &&
                     (std::isdigit(static_cast<unsigned char>(m_text[m_at + 1])) != 0 ||
                      ((m_text[m_at + 1] == '-' || m_text[m_at + 1] == '+') && m_at + 2 < m_text.size() &&
                       std::isdigit(static_cast<unsigned char>(m_text[m_at + 2])) != 0))) {
                digits.push_back('e');
                ++m_at;
                digits.push_back(m_text[m_at]);
            }
            else {
                break;
            }
            ++m_at;
        }
        if (!seenDigit)
            return std::nullopt;
        skipUnit();
        return std::strtod(digits.c_str(), nullptr);
    }

    std::string_view m_text;
    std::size_t m_at = 0;
};

} // namespace

std::optional<double> evaluateNumberExpression(std::string_view text)
{
    return NumberExpression(text).evaluate();
}

} // namespace engine::app
