#include "Utils/Logger.h"

namespace DirectxPlayground
{
ImguiLogger ImguiLogger::Logger{};

ImguiLogger::ImguiLogger()
{
    Clear();
}

void ImguiLogger::Draw(const char* title, bool* p_open /*= NULL*/)
{
    if (!ImGui::Begin(title, p_open))
    {
        ImGui::End();
        return;
    }

    if (ImGui::Checkbox("Auto-scroll", &m_autoScroll))
        if (m_autoScroll)
            m_scrollToBottom = true;
    ImGui::SameLine();
    bool clear = ImGui::Button("Clear");
    ImGui::SameLine();
    bool copy = ImGui::Button("Copy");

    ImGui::Separator();
    ImGui::BeginChild("scrolling", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    if (clear)
        Clear();
    if (copy)
        ImGui::LogToClipboard();

    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0, 0));
    const char* buf = m_textBuffer.begin();
    const char* buf_end = m_textBuffer.end();

    // The simplest and easy way to display the entire buffer:
    //   ImGui::TextUnformatted(buf_begin, buf_end);
    // And it'll just work. TextUnformatted() has specialization for large blob of text and will fast-forward to skip non-visible lines.
    // Here we instead demonstrate using the clipper to only process lines that are within the visible area.
    // If you have tens of thousands of items and their processing cost is non-negligible, coarse clipping them on your side is recommended.
    // Using ImGuiListClipper requires A) random access into your data, and B) items all being the  same height,
    // both of which we can handle since we an array pointing to the beginning of each line of text.
    // When using the filter (in the block of code above) we don't have random access into the data to display anymore, which is why we don't use the clipper.
    // Storing or skimming through the search result would make it possible (and would be recommended if you want to search through tens of thousands of entries)
    ImGuiListClipper clipper;
    clipper.Begin(m_lineOffsets.Size);
    while (clipper.Step())
    {
        for (int line_no = clipper.DisplayStart; line_no < clipper.DisplayEnd; line_no++)
        {
            const char* line_start = buf + m_lineOffsets[line_no];
            const char* line_end = (line_no + 1 < m_lineOffsets.Size) ? (buf + m_lineOffsets[line_no + 1] - 1) : buf_end;
            ImGui::TextUnformatted(line_start, line_end);
        }
    }
    clipper.End();

    ImGui::PopStyleVar();

    if (m_scrollToBottom)
        ImGui::SetScrollHereY(1.0f);
    m_scrollToBottom = false;
    ImGui::EndChild();
    ImGui::End();
}

void ImguiLogger::AddLogInternal(const char* fmt, ...)
{
    int old_size = m_textBuffer.size();
    va_list args;
    va_start(args, fmt);
    m_textBuffer.appendfv(fmt, args);
    va_end(args);
    for (int new_size = m_textBuffer.size(); old_size < new_size; old_size++)
        if (m_textBuffer[old_size] == '\n')
            m_lineOffsets.push_back(old_size + 1);
    if (m_autoScroll)
        m_scrollToBottom = true;
}
}