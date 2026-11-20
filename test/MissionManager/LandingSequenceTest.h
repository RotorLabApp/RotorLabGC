/****************************************************************************
 *
 * (c) 2009-2020 QGROUNDCONTROL PROJECT <http://www.qgroundcontrol.org>
 *
 * QGroundControl is licensed according to the terms in the file
 * COPYING.md in the root of the source code directory.
 *
 ****************************************************************************/

// RotorLab GC 5.1.2 (G75): a landing pattern's lastSequenceNumber must match the items it
// emits, including the DO_CHANGE_SPEED item added when useDoChangeSpeed is on.

#pragma once

#include "UnitTest.h"

class LandingSequenceTest : public UnitTest
{
    Q_OBJECT

protected:
    void init() final;
    void cleanup() final;

private slots:
    void _sequenceMatchesEmittedItems_data();
    void _sequenceMatchesEmittedItems();
};
