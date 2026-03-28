#pragma once

#include <windows.h>

#include "sonotide/result.h"

namespace sonotide::detail::win {

/// RAII-обёртка над COM apartment для потоков, которые работают с WASAPI и MF.
class com_scope {
public:
    /// Создаёт пустую scope-обёртку без активированной COM-сессии.
    com_scope() = default;
    /// Снимает COM-инициализацию, если этот scope её действительно выполнил.
    ~com_scope();

    /// Запускает `CoInitializeEx` в multithreaded-режиме и переводит результат в `sonotide::result`.
    com_scope(const com_scope&) = delete;
    /// `com_scope` не копируется, потому что владеет состоянием инициализации.
    com_scope& operator=(const com_scope&) = delete;

    /// Инициализирует текущий поток как multithreaded COM apartment.
    [[nodiscard]] result<void> initialize_multithreaded();

private:
    /// Фиксирует, должен ли деструктор вызвать `CoUninitialize`.
    bool should_uninitialize_ = false;
};

}  // namespace sonotide::detail::win
