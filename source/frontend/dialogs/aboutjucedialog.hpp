/*
 * Carla plugin host
 * Copyright (C) 2011-2022 Filipe Coelho <falktx@falktx.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * For a full copy of the GNU General Public License see the doc/GPL.txt file.
 */

#pragma once

#ifdef __clang__
# pragma clang diagnostic push
# pragma clang diagnostic ignored "-Wdeprecated-copy-with-user-provided-copy"
# pragma clang diagnostic ignored "-Wdeprecated-register"
#elif defined(__GNUC__) && __GNUC__ >= 8
# pragma GCC diagnostic push
# pragma GCC diagnostic ignored "-Wclass-memaccess"
# pragma GCC diagnostic ignored "-Wdeprecated-copy"
#endif

#include <QtWidgets/QDialog>

#ifdef __clang__
# pragma clang diagnostic pop
#elif defined(__GNUC__) && __GNUC__ >= 8
# pragma GCC diagnostic pop
#endif

#include "CarlaDefines.h"

// --------------------------------------------------------------------------------------------------------------------
// About JUCE dialog

class AboutJuceDialog : public QDialog
{
    struct Self;
    Self& self;

    // ----------------------------------------------------------------------------------------------------------------

public:
    explicit AboutJuceDialog(QWidget* parent);
    ~AboutJuceDialog() override;
};

// --------------------------------------------------------------------------------------------------------------------

extern "C" {

CARLA_API void carla_frontend_createAndExecAboutJuceDialog(void* parent);

}

// --------------------------------------------------------------------------------------------------------------------
