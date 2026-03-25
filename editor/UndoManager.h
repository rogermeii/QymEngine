#pragma once
#include <string>
#include <deque>
#include <functional>

namespace QymEngine {

class Scene;

class UndoManager {
public:
    static constexpr int MAX_UNDO_STEPS = 50;

    using SerializeFn = std::function<std::string()>;
    using DeserializeFn = std::function<void(const std::string&)>;

    void init(SerializeFn serialize, DeserializeFn deserialize) {
        m_serialize = std::move(serialize);
        m_deserialize = std::move(deserialize);
        m_undoStack.clear();
        m_redoStack.clear();
    }

    // Call before a destructive operation to save current state
    void saveState() {
        if (!m_serialize) return;
        std::string state = m_serialize();
        m_undoStack.push_back(std::move(state));
        if (static_cast<int>(m_undoStack.size()) > MAX_UNDO_STEPS)
            m_undoStack.pop_front();
        // New action clears redo stack
        m_redoStack.clear();
    }

    bool canUndo() const { return !m_undoStack.empty(); }
    bool canRedo() const { return !m_redoStack.empty(); }

    void undo() {
        if (!canUndo() || !m_serialize || !m_deserialize) return;
        // Save current state to redo stack
        m_redoStack.push_back(m_serialize());
        // Restore previous state
        std::string prev = m_undoStack.back();
        m_undoStack.pop_back();
        m_deserialize(prev);
    }

    void redo() {
        if (!canRedo() || !m_serialize || !m_deserialize) return;
        // Save current state to undo stack
        m_undoStack.push_back(m_serialize());
        // Restore next state
        std::string next = m_redoStack.back();
        m_redoStack.pop_back();
        m_deserialize(next);
    }

private:
    SerializeFn m_serialize;
    DeserializeFn m_deserialize;
    std::deque<std::string> m_undoStack;
    std::deque<std::string> m_redoStack;
};

} // namespace QymEngine
