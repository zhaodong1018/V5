// Copyright Epic Games, Inc. All Rights Reserved.

//------------------------------------------------------------------------------
// <auto-generated />
//
// This file was automatically generated by SWIG (http://www.swig.org).
// Version 4.0.1
//
// Do not make changes to this file unless you know what you are doing--modify
// the SWIG interface file instead.
//------------------------------------------------------------------------------


public enum EDatasmithFacadeTransformChannels {
  None = 0x000,
  TranslationX = 0x001,
  TranslationY = 0x002,
  TranslationZ = 0x004,
  Translation = TranslationX|TranslationY|TranslationZ,
  RotationX = 0x008,
  RotationY = 0x010,
  RotationZ = 0x020,
  Rotation = RotationX|RotationY|RotationZ,
  ScaleX = 0x040,
  ScaleY = 0x080,
  ScaleZ = 0x100,
  Scale = ScaleX|ScaleY|ScaleZ,
  All = Translation|Rotation|Scale
}
