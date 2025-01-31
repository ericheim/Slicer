/*==============================================================================

  Program: 3D Slicer

  Portions (c) Copyright Brigham and Women's Hospital (BWH) All Rights Reserved.

  See COPYRIGHT.txt
  or http://www.slicer.org/copyright/copyright.txt for details.

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

==============================================================================*/

#include "vtkMRMLMarkupsNode.h"

// MRML includes
#include "vtkCurveGenerator.h"
#include "vtkMRMLMarkupsFiducialStorageNode.h"
#include "vtkMRMLMarkupsDisplayNode.h"
#include "vtkMRMLMarkupsStorageNode.h"
#include "vtkMRMLTransformNode.h"

// Slicer MRML includes
#include "vtkMRMLScene.h"

// VTK includes
#include <vtkAbstractTransform.h>
#include <vtkBitArray.h>
#include <vtkBoundingBox.h>
#include <vtkCallbackCommand.h>
#include <vtkFrenetSerretFrame.h>
#include <vtkGeneralTransform.h>
#include <vtkMatrix4x4.h>
#include <vtkNew.h>
#include <vtkObjectFactory.h>
#include <vtkPointData.h>
#include <vtkPolyData.h>
#include <vtkStringArray.h>
#include <vtkTransformPolyDataFilter.h>
#include <vtkTrivialProducer.h>

// STD includes
#include <sstream>
#include <algorithm>

//----------------------------------------------------------------------------
vtkMRMLNodeNewMacro(vtkMRMLMarkupsNode);

//----------------------------------------------------------------------------
vtkMRMLMarkupsNode::vtkMRMLMarkupsNode()
{
  this->TextList = vtkSmartPointer<vtkStringArray>::New();
  this->Locked = 0;

  this->CurveClosed = false;

  this->RequiredNumberOfControlPoints = 0;
  this->MaximumNumberOfControlPoints = 0;
  this->MarkupLabelFormat = std::string("%N-%d");
  this->LastUsedControlPointNumber = 0;
  this->CenterPos.Set(0,0,0);

  this->CurveInputPoly = vtkSmartPointer<vtkPolyData>::New();
  vtkNew<vtkPoints> curveInputPoints;
  this->CurveInputPoly->SetPoints(curveInputPoints);

  this->CurvePoly = vtkSmartPointer<vtkPolyData>::New();
  vtkNew<vtkPoints> curvePoints;
  this->CurvePoly->SetPoints(curvePoints);
  vtkNew<vtkCellArray> lineCellArray;
  this->CurvePoly->SetLines(lineCellArray);

  this->CurveGenerator = vtkSmartPointer<vtkCurveGenerator>::New();
  this->CurveGenerator->SetInputPoints(curveInputPoints);
  this->CurveGenerator->SetOutputPoints(curvePoints);
  this->CurveGenerator->SetCurveTypeToLinearSpline();
  this->CurveGenerator->SetNumberOfPointsPerInterpolatingSegment(1);
  this->CurveGenerator->AddObserver(vtkCommand::ModifiedEvent, this->MRMLCallbackCommand);

  vtkNew<vtkTrivialProducer> curvePointConnector; // allows connecting a data object to pipeline input
  curvePointConnector->SetOutput(this->CurvePoly);

  this->CurvePolyToWorldTransform = vtkSmartPointer<vtkGeneralTransform>::New();

  this->CurvePolyToWorldTransformer = vtkSmartPointer<vtkTransformPolyDataFilter>::New();
  this->CurvePolyToWorldTransformer->SetInputConnection(curvePointConnector->GetOutputPort());
  this->CurvePolyToWorldTransformer->SetTransform(this->CurvePolyToWorldTransform);

  this->CurveCoordinateSystemGeneratorWorld = vtkSmartPointer<vtkFrenetSerretFrame>::New();
  // Curve coordinate system is computed at the very end of the pipeline so that it is only computed
  // if needed (it is not recomputed when a control point or the world transformation is modified).
  this->CurveCoordinateSystemGeneratorWorld->SetInputConnection(this->CurvePolyToWorldTransformer->GetOutputPort());

  this->TransformedCurvePolyLocator = vtkSmartPointer<vtkPointLocator>::New();
}

//----------------------------------------------------------------------------
vtkMRMLMarkupsNode::~vtkMRMLMarkupsNode()
{
  this->CurveGenerator->RemoveObserver(this->MRMLCallbackCommand);
  this->RemoveAllControlPoints();
}

//----------------------------------------------------------------------------
void vtkMRMLMarkupsNode::WriteXML(ostream& of, int nIndent)
{
  Superclass::WriteXML(of,nIndent);

  vtkMRMLWriteXMLBeginMacro(of);
  vtkMRMLWriteXMLBooleanMacro(locked, Locked);
  vtkMRMLWriteXMLStdStringMacro(markupLabelFormat, MarkupLabelFormat);
  vtkMRMLWriteXMLEndMacro();

  int textLength = static_cast<int>(this->TextList->GetNumberOfValues());
  for (int i = 0 ; i < textLength; i++)
    {
    of << " textList" << i << "=\"" << this->TextList->GetValue(i) << "\"";
    }
}

//----------------------------------------------------------------------------
void vtkMRMLMarkupsNode::ReadXMLAttributes(const char** atts)
{
  int disabledModify = this->StartModify();

  this->RemoveAllControlPoints();
  this->RemoveAllMeasurements();

  Superclass::ReadXMLAttributes(atts);

  vtkMRMLReadXMLBeginMacro(atts);
  vtkMRMLReadXMLBooleanMacro(locked, Locked);
  vtkMRMLReadXMLStdStringMacro(markupLabelFormat, MarkupLabelFormat);
  vtkMRMLReadXMLEndMacro();

  /* TODO: read measurements
  const char* attName;
  const char* attValue;
  while (*atts != nullptr)
    {
    attName = *(atts++);
    attValue = *(atts++);
    if (!strncmp(attName, "textList", 9))
      {
      this->AddText(attValue);
      }
    }
  */

  this->EndModify(disabledModify);
}

//----------------------------------------------------------------------------
void vtkMRMLMarkupsNode::Copy(vtkMRMLNode *anode)
{
  vtkMRMLMarkupsNode *node = vtkMRMLMarkupsNode::SafeDownCast(anode);
  if (!node)
    {
    return;
    }

  int disabledModify = this->StartModify();

  Superclass::Copy(anode);

  vtkMRMLCopyBeginMacro(anode);
  vtkMRMLCopyBooleanMacro(Locked);
  vtkMRMLCopyStdStringMacro(MarkupLabelFormat);
  vtkMRMLCopyEndMacro();

  this->TextList->DeepCopy(node->TextList);

  this->CurveInputPoly->GetPoints()->DeepCopy(node->CurveInputPoly->GetPoints());
  this->UpdateCurvePolyFromCurveInputPoly();

  // set max number of markups after adding the new ones
  this->LastUsedControlPointNumber = node->LastUsedControlPointNumber;

  this->CurveClosed = node->CurveClosed;

  // BUG: When fiducial nodes appear in scene views as of Slicer 4.1 the per
  // fiducial information (visibility, position etc) is saved to the file on
  // disk and not read, so the scene view copy of a fiducial node doesn't have
  // any fiducials in it. This work around prevents the main scene fiducial
  // list from being cleared of points and then not repopulated.
  // TBD: if scene view node reading xml triggers reading the data from
  // storage nodes, this should no longer be necessary.
  if (this->Scene &&
      this->Scene->IsRestoring())
    {
    if (this->GetNumberOfControlPoints() != 0 &&
        node->GetNumberOfControlPoints() == 0)
      {
      // just return for now
      vtkWarningMacro("MarkupsNode Copy: Scene view is restoring and list to restore is empty, skipping copy of points");
      return;
      }
    }

  this->RemoveAllControlPoints();
  int numMarkups = node->GetNumberOfControlPoints();
  for (int n = 0; n < numMarkups; n++)
    {
    ControlPoint* controlPoint = node->GetNthControlPoint(n);
    ControlPoint* controlPointCopy = new ControlPoint;
    (*controlPointCopy) = (*controlPoint);
    this->AddControlPoint(controlPointCopy);
    }

  this->EndModify(disabledModify);
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::ProcessMRMLEvents(vtkObject *caller,
                                           unsigned long event,
                                           void *callData)
{
  if (caller != nullptr && event == vtkMRMLTransformableNode::TransformModifiedEvent)
    {
    vtkMRMLTransformNode::GetTransformBetweenNodes(this->GetParentTransformNode(), nullptr, this->CurvePolyToWorldTransform);
    }
  else if (caller == this->CurveGenerator.GetPointer())
    {
    this->UpdateCurvePolyFromCurveInputPoly();
    int n = -1;
    this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
    }
  Superclass::ProcessMRMLEvents(caller, event, callData);
}

//----------------------------------------------------------------------------
void vtkMRMLMarkupsNode::PrintSelf(ostream& os, vtkIndent indent)
{
  Superclass::PrintSelf(os,indent);

  vtkMRMLPrintBeginMacro(os, indent);
  vtkMRMLPrintBooleanMacro(Locked);
  vtkMRMLPrintStdStringMacro(MarkupLabelFormat);
  vtkMRMLPrintEndMacro();

  os << indent << "MaximumNumberOfControlPoints: ";
  if (this->MaximumNumberOfControlPoints>0)
    {
    os << this->MaximumNumberOfControlPoints << "\n";
    }
  else
    {
    os << "unlimited\n";
    }
  os << indent << "RequiredNumberOfControlPoints: ";
  if (this->RequiredNumberOfControlPoints>0)
    {
    os << this->RequiredNumberOfControlPoints << "\n";
    }
  else
    {
    os << "unlimited\n";
    }
  os << indent << "NumberOfControlPoints: " << this->GetNumberOfControlPoints() << "\n";

  for (int controlPointIndex = 0; controlPointIndex < this->GetNumberOfControlPoints(); controlPointIndex++)
    {
    ControlPoint* controlPoint = this->GetNthControlPoint(controlPointIndex);
    if (!controlPoint)
      {
      continue;
      }
    os << indent << "Control Point " << controlPointIndex << ":\n";
    os << indent << "ID = " << controlPoint->ID.c_str() << "\n";
    os << indent << "Label = " << controlPoint->Label.c_str() << "\n";
    os << indent << "Description = " << controlPoint->Description.c_str() << "\n";
    os << indent << "Associated node id = " << controlPoint->AssociatedNodeID.c_str() << "\n";
    os << indent << "Selected = " << controlPoint->Selected << "\n";
    os << indent << "Locked = " << controlPoint->Locked << "\n";
    os << indent << "Visibility = " << controlPoint->Visibility << "\n";
    os << indent << "PositionStatus : " << controlPoint->PositionStatus << "\n";
    os << indent << "Position : " << controlPoint->Position[0] << ", " <<
          controlPoint->Position[1] << ", " << controlPoint->Position[2] << "\n";
    os << indent << "Orientation = ";
    for (int i = 0; i < 9; i++)
      {
      if (i > 0)
        {
        os << ",  ";
        }
      os << controlPoint->OrientationMatrix[i];
      }
    os << "\n";
    }

  os << indent << "Measurements: ";

  if  (this->GetNumberOfMeasurements()>0)
    {
    os << std::endl;
    for (int measurementIndex = 0; measurementIndex < this->GetNumberOfMeasurements(); measurementIndex++)
      {
      vtkMRMLMeasurement* m = this->GetNthMeasurement(measurementIndex);
      os << indent << m->GetName() << ": " << m->GetValueWithUnitsAsPrintableString() << std::endl;
      }
    }
  else
    {
    os << indent << "None" << std::endl;
    }

}

//----------------------------------------------------------------------------
void vtkMRMLMarkupsNode::RemoveAllControlPoints()
{
  if (this->ControlPoints.empty())
    {
    // no control points to remove
    return;
    }

  bool definedPointsExisted = false;
  for(unsigned int i = 0; i < this->ControlPoints.size(); i++)
    {
    if (this->ControlPoints[i]->PositionStatus == vtkMRMLMarkupsNode::PositionDefined)
      {
      definedPointsExisted = true;
      }
    delete this->ControlPoints[i];
    }

  this->ControlPoints.clear();

  this->CurveInputPoly->GetPoints()->Reset();
  this->CurveInputPoly->GetPoints()->Squeeze();
  this->CurvePoly->GetPoints()->Reset();
  this->CurvePoly->GetPoints()->Squeeze();
  this->CurvePoly->GetLines()->Reset();
  this->CurvePoly->GetLines()->Squeeze();

  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointRemovedEvent);
  if (definedPointsExisted)
    {
    this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointPositionUndefinedEvent);
    }
}

//-------------------------------------------------------------------------
vtkMRMLStorageNode* vtkMRMLMarkupsNode::CreateDefaultStorageNode()
{
  vtkMRMLScene* scene = this->GetScene();
  if (scene == nullptr)
    {
    vtkErrorMacro("CreateDefaultStorageNode failed: scene is invalid");
    return nullptr;
    }
  // By default we could store points in the scene (especially for lines
  // and angles), but for now we always store in an fcsv file.
  return vtkMRMLStorageNode::SafeDownCast(
    scene->CreateNodeByClass("vtkMRMLMarkupsFiducialStorageNode"));
}

//-------------------------------------------------------------------------
void vtkMRMLMarkupsNode::CreateDefaultDisplayNodes()
{
  if (this->GetDisplayNode() != nullptr &&
    vtkMRMLMarkupsDisplayNode::SafeDownCast(this->GetDisplayNode()) != nullptr)
    {
    // display node already exists
    return;
    }
  if (this->GetScene() == nullptr)
    {
    vtkErrorMacro("vtkMRMLMarkupsNode::CreateDefaultDisplayNodes failed: scene is invalid");
    return;
    }
  vtkMRMLMarkupsDisplayNode* dispNode = vtkMRMLMarkupsDisplayNode::SafeDownCast(
    this->GetScene()->AddNewNodeByClass("vtkMRMLMarkupsDisplayNode"));
  this->SetAndObserveDisplayNodeID(dispNode->GetID());
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetLocked(int locked)
{
  if (this->Locked == locked)
    {
    return;
    }
  this->Locked = locked;

  this->Modified();
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::LockModifiedEvent);
}

//---------------------------------------------------------------------------
vtkMRMLMarkupsDisplayNode *vtkMRMLMarkupsNode::GetMarkupsDisplayNode()
{
  vtkMRMLDisplayNode *displayNode = this->GetDisplayNode();
  if (displayNode &&
      displayNode->IsA("vtkMRMLMarkupsDisplayNode"))
    {
    return vtkMRMLMarkupsDisplayNode::SafeDownCast(displayNode);
    }
  return nullptr;
}

//---------------------------------------------------------------------------
bool vtkMRMLMarkupsNode::ControlPointExists(int n)
{
  if (n < 0 || n >= this->GetNumberOfControlPoints())
    {
    return false;
    }
  return (this->ControlPoints[static_cast<size_t>(n)] != nullptr);
}

//---------------------------------------------------------------------------
vtkMRMLMarkupsNode::ControlPoint* vtkMRMLMarkupsNode::GetNthControlPointCustomLog(int n, const char* failedMethodName)
{
  if (n < 0 || n >= this->GetNumberOfControlPoints())
    {
      vtkErrorMacro("vtkMRMLMarkupsNode::" << failedMethodName << " failed: control point " <<
        n << " does not exist");
    return nullptr;
    }

  ControlPoint* controlPoint = this->ControlPoints[static_cast<size_t>(n)];
  if (!controlPoint)
    {
    vtkErrorMacro("vtkMRMLMarkupsNode::" << failedMethodName << " failed: control point " <<
      n << " is invalid");
    }

  return controlPoint;
}

//---------------------------------------------------------------------------
int vtkMRMLMarkupsNode::GetNumberOfControlPoints()
{
  return static_cast<int> (this->ControlPoints.size());
}

//---------------------------------------------------------------------------
vtkMRMLMarkupsNode::ControlPoint* vtkMRMLMarkupsNode::GetNthControlPoint(int n)
{
  if (n < 0 || n >= this->GetNumberOfControlPoints())
    {
    vtkErrorMacro("vtkMRMLMarkupsNode::GetNthControlPoint failed: control point " <<
      n << " does not exist");
    return nullptr;
    }

  ControlPoint* controlPoint = this->ControlPoints[static_cast<size_t>(n)];
  if (!controlPoint)
    {
    vtkErrorMacro("vtkMRMLMarkupsNode::GetNthControlPoint failed: control point " <<
      n << " is invalid");
    }

  return controlPoint;
}

//-----------------------------------------------------------
std::vector< vtkMRMLMarkupsNode::ControlPoint* > * vtkMRMLMarkupsNode::GetControlPoints()
{
  return &this->ControlPoints;
}

//-----------------------------------------------------------
int vtkMRMLMarkupsNode::AddControlPoint(ControlPoint *controlPoint)
{
  if (this->MaximumNumberOfControlPoints != 0 &&
      this->GetNumberOfControlPoints() + 1 > this->MaximumNumberOfControlPoints)
    {
    vtkErrorMacro("AddNControlPoints: number of points major than maximum number of control points allowed.");
    return -1;
    }

  // generate a unique id based on list policy
  if (controlPoint->ID.empty())
    {
    controlPoint->ID = this->GenerateUniqueControlPointID();
    }
  this->LastUsedControlPointNumber++;
  if (controlPoint->Label.empty())
    {
    controlPoint->Label = this->GenerateControlPointLabel(this->LastUsedControlPointNumber);
    }

  this->ControlPoints.push_back(controlPoint);

  // Add point to CurveInputPoly
  // TODO: set point mask based on PositionStatus
  this->CurveInputPoly->GetPoints()->InsertNextPoint(controlPoint->Position);
  this->CurveInputPoly->GetPoints()->Modified();
  this->UpdateCurvePolyFromCurveInputPoly();

  int controlPointIndex = this->GetNumberOfControlPoints() - 1;
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointAddedEvent,  static_cast<void*>(&controlPointIndex));
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&controlPointIndex));
  if (controlPoint->PositionStatus == vtkMRMLMarkupsNode::PositionDefined)
    {
    this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointPositionDefinedEvent, static_cast<void*>(&controlPointIndex));
    }
  return controlPointIndex;
}

//-----------------------------------------------------------
int vtkMRMLMarkupsNode::AddNControlPoints(int n, std::string label /*=std::string()*/, vtkVector3d* point /*=nullptr*/)
{
  int controlPointIndex = -1;
  if (n < 0)
    {
    vtkErrorMacro("AddNControlPoints: invalid number of points " << n);
    return controlPointIndex;
    }

  if (this->MaximumNumberOfControlPoints != 0 &&  n > this->MaximumNumberOfControlPoints)
    {
    vtkErrorMacro("AddNControlPoints: number of points " << n <<
                  " major than maximum number of control points allowed : " << this->MaximumNumberOfControlPoints);
    return controlPointIndex;
    }

  for (int i = 0; i < n; i++)
    {
    ControlPoint *controlPoint = new ControlPoint;
    controlPoint->Label = label;
    if (point != nullptr)
      {
      controlPoint->Position[0] = point->GetX();
      controlPoint->Position[1] = point->GetY();
      controlPoint->Position[2] = point->GetZ();
      controlPoint->PositionStatus = PositionDefined;
      }
    else
      {
      controlPoint->PositionStatus = PositionUndefined;
      }
    controlPointIndex = this->AddControlPoint(controlPoint);
    }

  return controlPointIndex;
}

//-----------------------------------------------------------
int vtkMRMLMarkupsNode::AddControlPointWorld(vtkVector3d pointWorld, std::string label /*=std::string()*/)
{
  vtkVector3d point;
  this->TransformPointFromWorld(pointWorld, point);
  return this->AddNControlPoints(1, label, &point);
}

//-----------------------------------------------------------
int vtkMRMLMarkupsNode::AddControlPoint(vtkVector3d point, std::string label /*=std::string()*/)
{
  return this->AddNControlPoints(1, label, &point);
}

//-----------------------------------------------------------
vtkVector3d vtkMRMLMarkupsNode::GetNthControlPointPositionVector(int pointIndex)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(pointIndex, "GetNthControlPointPositionVector");
  if (!controlPoint)
    {
    return vtkVector3d(0, 0, 0);
    }
  return vtkVector3d(controlPoint->Position);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::GetNthControlPointPosition(int pointIndex, double point[3])
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(pointIndex, "GetNthControlPointPosition");
  if (!controlPoint)
    {
    point[0] = 0.0;
    point[1] = 0.0;
    point[2] = 0.0;
    return;
    }

  double* position = controlPoint->Position;
  point[0] = position[0];
  point[1] = position[1];
  point[2] = position[2];
}

//-----------------------------------------------------------
double* vtkMRMLMarkupsNode::GetNthControlPointPosition(int pointIndex)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(pointIndex, "GetNthControlPointPosition");
  if (!controlPoint)
    {
    return nullptr;
    }

  return controlPoint->Position;
}

//-----------------------------------------------------------
int vtkMRMLMarkupsNode::GetNthControlPointPositionWorld(int pointIndex, double worldxyz[3])
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(pointIndex, "GetNthControlPointPositionWorld");
  if (!controlPoint)
    {
    return 0;
    }
/*
  this->CurvePolyToWorldTransformer->Update();
  vtkPoints* pointsWorld = this->CurvePolyToWorldTransformer->GetOutput()->GetPoints();
  pointsWorld->GetPoint(pointIndex, worldxyz);
*/
  this->TransformPointToWorld(controlPoint->Position, worldxyz);
  return 1;
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::RemoveNthControlPoint(int pointIndex)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(pointIndex, "RemoveNthControlPoint");
  if (!controlPoint)
    {
    return;
    }

  // Allow reusing last control point number (to prevent continuously
  // incrementing the number in the control point's name when adding/removing preview points).
  std::string lastAutoGeneratedLabel = this->GenerateControlPointLabel(this->LastUsedControlPointNumber);
  if (lastAutoGeneratedLabel == this->GetNthControlPointLabel(pointIndex))
    {
    this->LastUsedControlPointNumber--;
    }

  bool positionWasDefined = (this->ControlPoints[static_cast<unsigned int>(pointIndex)]->PositionStatus == vtkMRMLMarkupsNode::PositionDefined);

  delete this->ControlPoints[static_cast<unsigned int> (pointIndex)];
  this->ControlPoints.erase(this->ControlPoints.begin() + pointIndex);

  this->UpdateCurvePolyFromControlPoints();

  if (positionWasDefined)
    {
    this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointPositionUndefinedEvent, static_cast<void*>(&pointIndex));
    }
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&pointIndex));
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointRemovedEvent, static_cast<void*>(&pointIndex));
}

//-----------------------------------------------------------
bool vtkMRMLMarkupsNode::InsertControlPoint(ControlPoint *controlPoint, int targetIndex)
{
  // generate a unique id based on list policy
  if (controlPoint->ID.empty())
    {
    controlPoint->ID = this->GenerateUniqueControlPointID();
    }

  /* do not generate labels for inserted points
  if (controlPoint->Label.empty())
    {
    controlPoint->Label = this->GenerateControlPointLabel(targetIndex);
    }
    */

  int listSize = this->GetNumberOfControlPoints();
  int destIndex = targetIndex;
  if (targetIndex < 0)
    {
    destIndex = 0;
    }
  else if (targetIndex > listSize)
    {
    destIndex = listSize;
    }

  std::vector < ControlPoint* >::iterator pos = this->ControlPoints.begin() + destIndex;
  std::vector < ControlPoint* >::iterator result = this->ControlPoints.insert(pos, controlPoint);

  this->UpdateCurvePolyFromControlPoints();

  // let observers know that a markup was added
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointAddedEvent, static_cast<void*>(&targetIndex));
  if (controlPoint->PositionStatus == vtkMRMLMarkupsNode::PositionDefined)
    {
    this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointPositionUndefinedEvent, static_cast<void*>(&targetIndex));
    }
  return true;
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::UpdateCurvePolyFromControlPoints()
{
  // Add points
  vtkPoints* points = this->CurveInputPoly->GetPoints();
  points->Reset();
  int numberOfControlPoints = this->GetNumberOfControlPoints();
  for (int i = 0; i < numberOfControlPoints; i++)
    {
    points->InsertNextPoint(this->ControlPoints[i]->Position);
    }
  points->Modified();

  this->UpdateCurvePolyFromCurveInputPoly();
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::UpdateCurvePolyFromCurveInputPoly()
{
  // curve generator is not a filter, it needs manual update
  this->CurveGenerator->Update();

  // Update lines: a single cell containing a line with point
  // indices: 0, 1, ..., last point (and an extra 0 if closed curve).
  vtkIdType numberOfPoints = this->CurvePoly->GetNumberOfPoints();
  vtkCellArray* lines = this->CurvePoly->GetLines();
  if (numberOfPoints > 1)
    {
    bool loop = (numberOfPoints > 2 && this->CurveClosed);
    vtkIdType numberOfCellPoints = (loop ? numberOfPoints + 1 : numberOfPoints);

    // Only regenerate indices if necessary
    bool needToUpdateLines = true;
    if (lines->GetNumberOfCells() == 1)
      {
      vtkIdType currentNumberOfCellPoints = 0;
      vtkIdType* currentCellPoints = nullptr;
      lines->GetCell(0, currentNumberOfCellPoints, currentCellPoints);

      if (currentNumberOfCellPoints == numberOfCellPoints)
        {
        needToUpdateLines = false;
        }
      }

    if (needToUpdateLines)
      {
      lines->Reset();
      lines->InsertNextCell(numberOfCellPoints);
      for (int i = 0; i < numberOfPoints; i++)
        {
        lines->InsertCellPoint(i);
        }
      if (loop)
        {
        lines->InsertCellPoint(0);
        }
      lines->Modified();
      }
    }
  else
    {
    lines->Reset();
    }
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SwapControlPoints(int m1, int m2)
{
  ControlPoint *controlPoint1 = this->GetNthControlPointCustomLog(m1, "SwapControlPoints");
  ControlPoint *controlPoint2 = this->GetNthControlPointCustomLog(m2, "SwapControlPoints");
  if (!controlPoint1 || !controlPoint2)
    {
    return;
    }

  // make a copy of the first control point
  ControlPoint controlPoint1Backup = *controlPoint1;
  // copy the second control point into the first
  *controlPoint1 = *controlPoint2;
  // and copy the backup of the first one into the second
  *controlPoint2 = controlPoint1Backup;

  this->UpdateCurvePolyFromControlPoints();

  // and let listeners know that two control points have changed
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&m1));
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&m2));
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointPositionFromPointer(const int pointIndex,
                                                               const double * pos)
{
  if (!pos)
    {
    vtkErrorMacro("SetNthControlPointFromPointer: invalid position pointer!");
    return;
    }

  this->SetNthControlPointPosition(pointIndex, pos[0], pos[1], pos[2]);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointPositionFromArray(const int pointIndex,
                                                             const double pos[3], int positionStatus/*=PositionDefined*/)
{
  this->SetNthControlPointPosition(pointIndex, pos[0], pos[1], pos[2], positionStatus);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointPosition(const int pointIndex,
  const double x, const double y, const double z, int positionStatus/*=PositionDefined*/)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(pointIndex, "SetNthControlPointPosition");
  if (!controlPoint)
    {
    return;
    }

  // TODO: return if no modification
  double* controlPointPosition = controlPoint->Position;
  controlPointPosition[0] = x;
  controlPointPosition[1] = y;
  controlPointPosition[2] = z;
  int oldPositionStatus = controlPoint->PositionStatus;
  controlPoint->PositionStatus = positionStatus;

  vtkPoints* points = this->CurveInputPoly->GetPoints();
  points->SetPoint(pointIndex, x, y, z);
  points->Modified();
  this->UpdateCurvePolyFromCurveInputPoly();

  // throw an event to let listeners know the position has changed
  int n = pointIndex;
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
  if (oldPositionStatus != PositionDefined && positionStatus == PositionDefined)
    {
    this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointPositionDefinedEvent, static_cast<void*>(&n));
    }
  else if (oldPositionStatus == PositionDefined && positionStatus != PositionDefined)
    {
    this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointPositionUndefinedEvent, static_cast<void*>(&n));
    }
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointPositionWorld(const int pointIndex,
                                                         const double x, const double y, const double z)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(pointIndex, "SetNthControlPointPositionWorld");
  if (!controlPoint)
    {
    return;
    }
  vtkVector3d markupxyz;
  TransformPointFromWorld(vtkVector3d(x,y,z), markupxyz);
  this->SetNthControlPointPosition(pointIndex, markupxyz[0], markupxyz[1], markupxyz[2]);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointPositionWorldFromArray(
  const int pointIndex, const double pos[3], int positionStatus/*=PositionDefined*/)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(pointIndex, "SetNthControlPointPositionWorldFromArray");
  if (!controlPoint)
    {
    return;
    }
  double markupxyz[3] = { 0.0 };
  TransformPointFromWorld(pos, markupxyz);
  this->SetNthControlPointPositionFromArray(pointIndex, markupxyz, positionStatus);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointPositionOrientationWorldFromArray(
  const int pointIndex, const double pos[3], const double orientationMatrix[9],
  const char* associatedNodeID, int positionStatus/*=PositionDefined*/)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(pointIndex, "SetNthControlPointPositionOrientationWorldFromArray");
  if (!controlPoint)
    {
    return;
    }
  // TODO: return if no modification
  this->TransformPointFromWorld(pos, controlPoint->Position);
  int oldPositionStatus = controlPoint->PositionStatus;
  controlPoint->PositionStatus = positionStatus;
  // TODO: transform orientation matrix to world
  std::copy_n(orientationMatrix, 9, controlPoint->OrientationMatrix);
  if (associatedNodeID)
    {
    controlPoint->AssociatedNodeID = associatedNodeID;
    }
  else
    {
    controlPoint->AssociatedNodeID.empty();
    }

  vtkPoints* points = this->CurveInputPoly->GetPoints();
  points->SetPoint(pointIndex, controlPoint->Position);
  points->Modified();
  this->UpdateCurvePolyFromCurveInputPoly();

  // throw an event to let listeners know the position has changed
  int n = pointIndex;
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
  if (oldPositionStatus != PositionDefined && positionStatus == PositionDefined)
  {
    this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointPositionDefinedEvent, static_cast<void*>(&n));
  }
  else if (oldPositionStatus == PositionDefined && positionStatus != PositionDefined)
  {
    this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointPositionUndefinedEvent, static_cast<void*>(&n));
  }
}

//-----------------------------------------------------------
vtkVector3d vtkMRMLMarkupsNode::GetCenterPositionVector()
{
  return this->CenterPos;
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::GetCenterPosition(double point[3])
{
  point[0] = this->CenterPos.GetX();
  point[1] = this->CenterPos.GetY();
  point[2] = this->CenterPos.GetZ();
}

//-----------------------------------------------------------
int vtkMRMLMarkupsNode::GetCenterPositionWorld(double worldxyz[3])
{
  vtkVector3d world;
  this->TransformPointToWorld(this->GetCenterPositionVector(), world);
  worldxyz[0] = world[0];
  worldxyz[1] = world[1];
  worldxyz[2] = world[2];
  return 1;
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetCenterPositionFromPointer(const double *pos)
{
  if (!pos)
    {
    vtkErrorMacro("SetCenterPositionFromPointer: invalid position pointer!");
    return;
    }

  this->SetCenterPosition(pos[0], pos[1], pos[2]);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetCenterPositionFromArray(const double pos[3])
{
  this->SetCenterPosition(pos[0], pos[1], pos[2]);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetCenterPosition(const double x, const double y, const double z)
{
  this->CenterPos.Set(x,y,z);
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::CenterPointModifiedEvent);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetCenterPositionWorld(const double x, const double y, const double z)
{
  vtkVector3d centerxyz;
  TransformPointFromWorld(vtkVector3d(x,y,z), centerxyz);
  this->SetCenterPosition(centerxyz[0], centerxyz[1], centerxyz[2]);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointOrientationFromPointer(int n, const double *orientation)
{
  if (!orientation)
    {
    vtkErrorMacro("Invalid orientation pointer!");
    return;
    }
  this->SetNthControlPointOrientation(n, orientation[0], orientation[1], orientation[2], orientation[3]);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointOrientationFromArray(int n, const double orientation[4])
{
  this->SetNthControlPointOrientation(n, orientation[0], orientation[1], orientation[2], orientation[3]);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointOrientation(int n, double w, double x, double y, double z)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "SetNthControlPointOrientation");
  if (!controlPoint)
    {
    return;
    }
  // TODO: return if no modification

  double wxyz[] = { w, x, y, z };
  vtkMRMLMarkupsNode::ConvertOrientationWXYZToMatrix(wxyz, controlPoint->OrientationMatrix);

  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::GetNthControlPointOrientation(int n, double orientation[4])
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointOrientation");
  if (!controlPoint)
    {
    return;
    }
  vtkMRMLMarkupsNode::ConvertOrientationMatrixToWXYZ(controlPoint->OrientationMatrix, orientation);
}

//-----------------------------------------------------------
double* vtkMRMLMarkupsNode::GetNthControlPointOrientationMatrix(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointOrientationMatrix");
  if (!controlPoint)
    {
    static double identity[9] = { 1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0 };
    return identity;
    }
  return controlPoint->OrientationMatrix;
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointOrientationMatrix(int n, double orientationMatrix[9])
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "SetNthControlPointOrientationMatrix");
  if (!controlPoint)
    {
    return;
    }
  double* controlPointOrientationMatrix = controlPoint->OrientationMatrix;
  for (int i = 0; i < 9; i++)
    {
    controlPointOrientationMatrix[i] = orientationMatrix[i];
    }
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointOrientationMatrixWorld(int n, double orientationMatrix[9])
{
  this->SetNthControlPointOrientationMatrix(n, orientationMatrix);
}

//-----------------------------------------------------------
double* vtkMRMLMarkupsNode::GetNthControlPointNormal(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointNormal");
  if (!controlPoint)
    {
    static double identity[3] = { 0.0, 0.0, 1.0 };
    return identity;
    }
  double* orientationMatrix = this->GetNthControlPoint(n)->OrientationMatrix;
  // OrientationMatrix contains z axis direction from index 6
  return orientationMatrix + 6;
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::GetNthControlPointNormalWorld(int n, double normalWorld[3])
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointNormalWorld");
  if (!controlPoint)
    {
    return;
    }
  this->CurvePolyToWorldTransform->TransformNormalAtPoint(
    &(controlPoint->Position[0]),
    &(controlPoint->OrientationMatrix[0])+6,
    normalWorld);
}

//-----------------------------------------------------------
vtkVector4d vtkMRMLMarkupsNode::GetNthControlPointOrientationVector(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointOrientationVector");
  if (!controlPoint)
    {
    return vtkVector4d(0, 0, 0, 0);
    }

  double orientationWXYZ[4] = { 0.0, 0.0, 0.0, 1.0 };
  vtkMRMLMarkupsNode::ConvertOrientationMatrixToWXYZ(controlPoint->OrientationMatrix, orientationWXYZ);

  vtkVector4d orientationXYZW;
  // Note the order difference: vtkVector4d stores orientation as xyzw
  orientationXYZW.SetX(orientationWXYZ[1]);
  orientationXYZW.SetY(orientationWXYZ[2]);
  orientationXYZW.SetZ(orientationWXYZ[3]);
  orientationXYZW.SetW(orientationWXYZ[0]);
  return orientationXYZW;
}

//-----------------------------------------------------------
std::string vtkMRMLMarkupsNode::GetNthControlPointAssociatedNodeID(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointAssociatedNodeID");
  if (!controlPoint)
    {
    return std::string("");
    }
  return controlPoint->AssociatedNodeID;
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointAssociatedNodeID(int n, std::string id)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "SetNthControlPointAssociatedNodeID");
  if (!controlPoint)
    {
    return;
    }
  if (controlPoint->AssociatedNodeID == id)
    {
    // no change
    return;
    }
  controlPoint->AssociatedNodeID = std::string(id.c_str());
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
}

//-----------------------------------------------------------
std::string vtkMRMLMarkupsNode::GetNthControlPointID(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointID");
  if (!controlPoint)
    {
    return std::string("");
    }
  return controlPoint->ID;
}

//-------------------------------------------------------------------------
int vtkMRMLMarkupsNode::GetNthControlPointIndexByID(const char* controlPointID)
{
  if (!controlPointID)
    {
    return -1;
    }
  for (int controlPointIndex = 0; controlPointIndex < this->GetNumberOfControlPoints(); controlPointIndex++)
    {
    ControlPoint *compareControlPoint = this->ControlPoints[controlPointIndex];
    if (compareControlPoint &&
        strcmp(compareControlPoint->ID.c_str(), controlPointID) == 0)
      {
      return controlPointIndex;
      }
    }
  return -1;
}

//-------------------------------------------------------------------------
vtkMRMLMarkupsNode::ControlPoint* vtkMRMLMarkupsNode::GetNthControlPointByID(const char* controlPointID)
{
  if (!controlPointID)
    {
    return nullptr;
    }
  int controlPointIndex = this->GetNthControlPointIndexByID(controlPointID);
  if (controlPointIndex < 0 || controlPointIndex >= this->GetNumberOfControlPoints())
    {
    return nullptr;
    }
  return this->GetNthControlPoint(controlPointIndex);
}

//-----------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointID(int n, std::string id)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "SetNthControlPointID");
  if (!controlPoint)
    {
    return;
    }
  if (controlPoint->ID.compare(id) == 0)
    {
    // no change
    return;
    }
  controlPoint->ID = id;
}

//---------------------------------------------------------------------------
bool vtkMRMLMarkupsNode::GetNthControlPointSelected(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointSelected");
  if (!controlPoint)
   {
   return false;
   }
 return controlPoint->Selected;
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointSelected(int n, bool flag)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "SetNthControlPointSelected");
  if (!controlPoint)
    {
    return;
    }
  if (controlPoint->Selected == flag)
    {
    // no change
    return;
    }
  controlPoint->Selected = flag;
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
}

//---------------------------------------------------------------------------
bool vtkMRMLMarkupsNode::GetNthControlPointLocked(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointLocked");
  if (!controlPoint)
    {
    return false;
    }
  return controlPoint->Locked;
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointLocked(int n, bool flag)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "SetNthControlPointLocked");
  if (!controlPoint)
    {
    return;
    }
  if (controlPoint->Locked == flag)
    {
    // no change
    return;
    }
  controlPoint->Locked = flag;
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
}

//---------------------------------------------------------------------------
bool vtkMRMLMarkupsNode::GetNthControlPointVisibility(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointVisibility");
  if (!controlPoint)
    {
    return false;
    }
  return controlPoint->Visibility;
}


//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointVisibility(int n, bool flag)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "SetNthControlPointVisibility");
  if (!controlPoint)
    {
    return;
    }
  if (controlPoint->Visibility == flag)
    {
    // no change
    return;
    }
  controlPoint->Visibility = flag;
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
}

//---------------------------------------------------------------------------
std::string vtkMRMLMarkupsNode::GetNthControlPointLabel(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointLabel");
  if (!controlPoint)
    {
    return std::string("");
    }
  return controlPoint->Label;
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointLabel(int n, std::string label)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "SetNthControlPointLabel");
  if (!controlPoint)
    {
    return;
    }
  if (!controlPoint->Label.compare(label))
    {
    // no change
    return;
    }
  controlPoint->Label = label;
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
}

//---------------------------------------------------------------------------
std::string vtkMRMLMarkupsNode::GetNthControlPointDescription(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointDescription");
  if (!controlPoint)
    {
    return std::string("");
    }
  return controlPoint->Description;
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthControlPointDescription(int n, std::string description)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "SetNthControlPointDescription");
  if (!controlPoint)
    {
    return;
    }
  if (!controlPoint->Description.compare(description))
    {
    // no change
    return;
    }
  controlPoint->Description = description;
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
}

//---------------------------------------------------------------------------
bool vtkMRMLMarkupsNode::CanApplyNonLinearTransforms()const
{
  return true;
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::ApplyTransform(vtkAbstractTransform* transform)
{
  int numControlPoints = this->GetNumberOfControlPoints();
  double xyzIn[3];
  double xyzOut[3];
  for (int controlpointIndex = 0; controlpointIndex < numControlPoints; controlpointIndex++)
    {
    this->GetNthControlPointPosition(controlpointIndex, xyzIn);
    transform->TransformPoint(xyzIn,xyzOut);
    this->SetNthControlPointPositionFromArray(controlpointIndex, xyzOut);
    }
  this->StorableModifiedTime.Modified();
  this->Modified();
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::
WriteCLI(std::vector<std::string>& commandLine, std::string prefix,
         int coordinateSystem, int multipleFlag)
{
  Superclass::WriteCLI(commandLine, prefix, coordinateSystem, multipleFlag);

  int numControlPoints = this->GetNumberOfControlPoints();

  // check if the coordinate system flag is set to LPS, otherwise assume RAS
  bool useLPS = false;
  if (coordinateSystem == 1)
    {
    useLPS = true;
    }

  // loop over the control points
  for (int m = 0; m < numControlPoints; m++)
    {
    // only use selected markups
    if (this->GetNthControlPointSelected(m))
      {
      std::stringstream ss;
      double point[3];
      this->GetNthControlPointPosition(m, point);
      if (useLPS)
        {
        point[0] = -point[0];
        point[1] = -point[1];
        }
      // write
      if (prefix.compare("") != 0)
        {
        commandLine.push_back(prefix);
        }
      // avoid scientific notation
      //ss.precision(5);
      //ss << std::fixed << point[0] << "," <<  point[1] << "," <<  point[2] ;
      ss << point[0] << "," <<  point[1] << "," <<  point[2];
      commandLine.push_back(ss.str());
      if (multipleFlag == 0)
        {
        // only print out one markup, but print out all the points in that markup
        // (if have a ruler, need to do 2 points)
        break;
        }
      }
    }
}

//---------------------------------------------------------------------------
bool vtkMRMLMarkupsNode::GetModifiedSinceRead()
{
  if (this->Superclass::GetModifiedSinceRead())
    {
    return true;
    }
  vtkPoints* points = this->CurveInputPoly->GetPoints();
  if (points != nullptr && points->GetMTime() > this->GetStoredTime())
    {
    return true;
    }
  return false;
}

//---------------------------------------------------------------------------
bool vtkMRMLMarkupsNode::ResetNthControlPointID(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "ResetNthControlPointID");
  if (!controlPoint)
    {
    return false;
    }
  this->SetNthControlPointID(n, this->GenerateUniqueControlPointID());
  return true;
}

//---------------------------------------------------------------------------
std::string vtkMRMLMarkupsNode::GenerateUniqueControlPointID()
{
  std::string id;
  int controlPointNumber = this->LastUsedControlPointNumber;
  // increment by one so as not to start with 0
  controlPointNumber++;
  // put the number in a string
  return std::to_string(controlPointNumber);
}

//---------------------------------------------------------------------------
std::string vtkMRMLMarkupsNode::GenerateControlPointLabel(int controlPointIndex)
{
  std::string formatString = this->ReplaceListNameInMarkupLabelFormat();
  char buf[128];
  buf[sizeof(buf) - 1] = 0; // make sure the string is zero-terminated
  snprintf(buf, sizeof(buf) - 1, formatString.c_str(), controlPointIndex);
  return std::string(buf);
}

//---------------------------------------------------------------------------
std::string vtkMRMLMarkupsNode::GetMarkupLabelFormat()
{
  return this->MarkupLabelFormat;
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetMarkupLabelFormat(std::string format)
{
  if (this->MarkupLabelFormat.compare(format) == 0)
    {
    return;
    }
  this->MarkupLabelFormat = format;

  this->Modified();
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::LabelFormatModifiedEvent);
}

//---------------------------------------------------------------------------
std::string vtkMRMLMarkupsNode::ReplaceListNameInMarkupLabelFormat()
{
  std::string newFormatString = this->MarkupLabelFormat;
  size_t replacePos = newFormatString.find("%N");
  if (replacePos != std::string::npos)
    {
    // replace the special character with the list name, or an empty string if
    // no list name is set
    std::string name;
    if (this->GetName() != nullptr)
      {
      name = std::string(this->GetName());
      }
    newFormatString.replace(replacePos, 2, name);
    }
  return newFormatString;
}

//----------------------------------------------------------------------
void vtkMRMLMarkupsNode::ConvertOrientationMatrixToWXYZ(const double orientationMatrix[9], double wxyz[4])
{
  if (!orientationMatrix || !wxyz)
    {
    return;
    }

  // adapted from vtkTransform::GetOrientationWXYZ

  double ortho[3][3];
  ortho[0][0] = orientationMatrix[0];
  ortho[0][1] = orientationMatrix[1];
  ortho[0][2] = orientationMatrix[2];
  ortho[1][0] = orientationMatrix[3];
  ortho[1][1] = orientationMatrix[4];
  ortho[1][2] = orientationMatrix[5];
  ortho[2][0] = orientationMatrix[6];
  ortho[2][1] = orientationMatrix[7];
  ortho[2][2] = orientationMatrix[8];

  if (vtkMath::Determinant3x3(ortho) < 0)
    {
    ortho[0][2] = -ortho[0][2];
    ortho[1][2] = -ortho[1][2];
    ortho[2][2] = -ortho[2][2];
    }

  vtkMath::Matrix3x3ToQuaternion(ortho, wxyz);

  // calc the return value wxyz
  double mag = sqrt(wxyz[1] * wxyz[1] + wxyz[2] * wxyz[2] + wxyz[3] * wxyz[3]);

  if (mag != 0.0)
    {
    wxyz[0] = 2.0 * vtkMath::DegreesFromRadians(atan2(mag, wxyz[0]));
    wxyz[1] /= mag;
    wxyz[2] /= mag;
    wxyz[3] /= mag;
    }
  else
    {
    wxyz[0] = 0.0;
    wxyz[1] = 0.0;
    wxyz[2] = 0.0;
    wxyz[3] = 1.0;
    }
}

//----------------------------------------------------------------------
void vtkMRMLMarkupsNode::ConvertOrientationWXYZToMatrix(double orientationWXYZ[4], double orientationMatrix[9])
{
  if (!orientationWXYZ || !orientationMatrix)
    {
    return;
    }

  // adapted from vtkTransformConcatenation::Rotate(double angle, double x, double y, double z)

  double angle = orientationWXYZ[0];
  double x = orientationWXYZ[1];
  double y = orientationWXYZ[2];
  double z = orientationWXYZ[3];

  if (angle == 0.0 || (x == 0.0 && y == 0.0 && z == 0.0))
    {
    orientationMatrix[0] = 1.0;
    orientationMatrix[1] = 0.0;
    orientationMatrix[2] = 0.0;
    orientationMatrix[3] = 0.0;
    orientationMatrix[4] = 1.0;
    orientationMatrix[5] = 0.0;
    orientationMatrix[6] = 0.0;
    orientationMatrix[7] = 0.0;
    orientationMatrix[8] = 1.0;
    return;
    }

  // convert to radians
  angle = vtkMath::RadiansFromDegrees(angle);

  // make a normalized quaternion
  double w = cos(0.5*angle);
  double f = sin(0.5*angle) / sqrt(x*x + y * y + z * z);
  x *= f;
  y *= f;
  z *= f;

  // convert the quaternion to a matrix
  double matrix[4][4];
  vtkMatrix4x4::Identity(*matrix);

  double ww = w * w;
  double wx = w * x;
  double wy = w * y;
  double wz = w * z;

  double xx = x * x;
  double yy = y * y;
  double zz = z * z;

  double xy = x * y;
  double xz = x * z;
  double yz = y * z;

  double s = ww - xx - yy - zz;

  orientationMatrix[0] = xx * 2 + s;    // (0,0)
  orientationMatrix[3] = (xy + wz) * 2; // (1,0)
  orientationMatrix[6] = (xz - wy) * 2; // (2,0)

  orientationMatrix[1] = (xy - wz) * 2; // (0,1)
  orientationMatrix[4] = yy * 2 + s;    // (1,1)
  orientationMatrix[7] = (yz + wx) * 2; // (2,1)

  orientationMatrix[2] = (xz + wy) * 2; // (0,2)
  orientationMatrix[5] = (yz - wx) * 2; // (1,2)
  orientationMatrix[8] = zz * 2 + s;    // (2,2)
}

//----------------------------------------------------------------------
vtkPoints* vtkMRMLMarkupsNode::GetCurvePointsWorld()
{
  vtkPolyData* curvePolyDataWorld = this->GetCurveWorld();
  if (!curvePolyDataWorld)
    {
    return nullptr;
    }
  return curvePolyDataWorld->GetPoints();
}

//----------------------------------------------------------------------
vtkPolyData* vtkMRMLMarkupsNode::GetCurveWorld()
{
  if (this->GetNumberOfControlPoints() < 1)
    {
    return nullptr;
    }
  this->CurvePolyToWorldTransformer->Update();
  vtkPolyData* curvePolyDataWorld = this->CurvePolyToWorldTransformer->GetOutput();
  this->TransformedCurvePolyLocator->SetDataSet(curvePolyDataWorld);
  return curvePolyDataWorld;
}

//----------------------------------------------------------------------
vtkAlgorithmOutput* vtkMRMLMarkupsNode::GetCurveWorldConnection()
{
  return this->CurvePolyToWorldTransformer->GetOutputPort();
}

//----------------------------------------------------------------------
int vtkMRMLMarkupsNode::GetControlPointIndexFromInterpolatedPointIndex(vtkIdType interpolatedPointIndex)
{
  if (this->CurveGenerator->IsInterpolatingCurve())
    {
    return int(floor(interpolatedPointIndex / this->CurveGenerator->GetNumberOfPointsPerInterpolatingSegment()));
    }
  if (this->CurveGenerator->GetPolynomialPointSortingMethod() == vtkCurveGenerator::SORTING_METHOD_MINIMUM_SPANNING_TREE_POSITION)
    {
    // If sorting is based on spanning tree then we can insert point anywhere (so we add to the end for simplicity).
    return this->GetNumberOfControlPoints();
    }
  // In case of approximating curves, there is no clear assignment between control points and curve points.
  vtkWarningMacro("vtkMRMLMarkupsNode::GetControlPointIndexFromInterpolatedPointIndex for non-interpolated"
    " curves, minimum spanning tree sorting is recommended");
  return this->GetNumberOfControlPoints();
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::GetRASBounds(double bounds[6])
{
  vtkBoundingBox box;
  box.GetBounds(bounds);

  int numberOfControlPoints = this->GetNumberOfControlPoints();
  if (numberOfControlPoints == 0)
    {
    return;
    }
  double markup_RAS[4] = { 0, 0, 0, 1 };

  for (int i = 0; i < numberOfControlPoints; i++)
    {
    this->GetNthControlPointPositionWorld(i, markup_RAS);
    box.AddPoint(markup_RAS);
    }
  box.GetBounds(bounds);
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::GetBounds(double bounds[6])
{
  vtkBoundingBox box;
  box.GetBounds(bounds);

  int numberOfControlPoints = this->GetNumberOfControlPoints();
  if (numberOfControlPoints == 0)
    {
    return;
    }
  double markupPos[4] = { 0, 0, 0 };

  for (int i = 0; i < numberOfControlPoints; i++)
    {
    this->GetNthControlPointPosition(i, markupPos);
    box.AddPoint(markupPos);
    }
  box.GetBounds(bounds);
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::GetMarkupPoint(int markupIndex, int pointIndex, double point[3])
{
  if (markupIndex == 0)
    {
    this->GetNthControlPointPosition(pointIndex, point);
    }
  else if (pointIndex == 0)
    {
    this->GetNthControlPointPosition(markupIndex, point);
    }
  else
    {
    vtkErrorMacro("vtkMRMLMarkupsNode::GetMarkupPoint failed: only one markup with mutiple control points is supported.");
    }
}

//---------------------------------------------------------------------------
int vtkMRMLMarkupsNode::GetNthControlPointPositionStatus(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "GetNthControlPointPositionStatus");
  if (!controlPoint)
    {
    return PositionUndefined;
    }
  return controlPoint->PositionStatus;
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::UnsetNthControlPointPosition(int n)
{
  ControlPoint *controlPoint = this->GetNthControlPointCustomLog(n, "UnsetNthControlPointPosition");
  if (!controlPoint)
    {
    return;
    }
  if (controlPoint->PositionStatus == PositionUndefined)
    {
    // no change
    return;
    }
  controlPoint->PositionStatus = PositionUndefined;
  this->InvokeCustomModifiedEvent(vtkMRMLMarkupsNode::PointModifiedEvent, static_cast<void*>(&n));
}

//---------------------------------------------------------------------------
int vtkMRMLMarkupsNode::GetNumberOfDefinedControlPoints()
{
  int numberOfDefinedControlPoints = 0;
  for (ControlPointsListType::iterator controlPointIt = this->ControlPoints.begin();
    controlPointIt != this->ControlPoints.end(); ++controlPointIt)
    {
    if ((*controlPointIt)->PositionStatus == PositionDefined)
      {
      numberOfDefinedControlPoints++;
      }
    }
  return numberOfDefinedControlPoints;
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::OnTransformNodeReferenceChanged(vtkMRMLTransformNode* transformNode)
{
  vtkMRMLTransformNode::GetTransformBetweenNodes(this->GetParentTransformNode(), nullptr, this->CurvePolyToWorldTransform);
  Superclass::OnTransformNodeReferenceChanged(transformNode);
}

//---------------------------------------------------------------------------
int vtkMRMLMarkupsNode::GetClosestControlPointIndexToPositionWorld(double pos[3])
{
  int numberOfControlPoints = this->GetNumberOfControlPoints();
  if (numberOfControlPoints <= 0)
    {
    return -1;
    }
  if (numberOfControlPoints == 1)
    {
    // there is one control point, so the closest one is the only one
    return 0;
    }
  vtkIdType indexOfClosestMarkup = -1;
  double closestDistanceSquare = 0;
  for (vtkIdType pointIndex = 0; pointIndex < numberOfControlPoints; pointIndex++)
    {
    double currentPos[4] = { 0.0, 0.0, 0.0, 1.0 };
    this->GetNthControlPointPositionWorld(pointIndex, currentPos);
    double distanceSquare = vtkMath::Distance2BetweenPoints(pos, currentPos);
    if (distanceSquare < closestDistanceSquare || indexOfClosestMarkup < 0)
      {
      indexOfClosestMarkup = pointIndex;
      closestDistanceSquare = distanceSquare;
      }
    }
  return indexOfClosestMarkup;
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::GetControlPointLabels(vtkStringArray* labels)
{
  if (!labels)
    {
    vtkErrorMacro("GetControlPointLabels failed: invalid labels");
    return;
    }
  int numberOfControlPoints = this->GetNumberOfControlPoints();
  labels->SetNumberOfValues(numberOfControlPoints);
  for (vtkIdType pointIndex = 0; pointIndex < numberOfControlPoints; pointIndex++)
    {
    labels->SetValue(pointIndex, this->GetNthControlPointLabel(pointIndex));
    }
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetControlPointPositionsWorld(vtkPoints* points)
{
  if (!points)
    {
    this->RemoveAllControlPoints();
    return;
    }
  int wasModified = this->StartModify();
  vtkIdType numberOfPoints = points->GetNumberOfPoints();
  for (vtkIdType pointIndex = 0; pointIndex < numberOfPoints; pointIndex++)
    {
    double* posWorld = points->GetPoint(pointIndex);
    if (pointIndex < this->GetNumberOfControlPoints())
      {
      // point already exists, just update it
      this->SetNthControlPointPositionWorldFromArray(pointIndex, posWorld);
      }
    else
      {
      // need to add a new point
      vtkMRMLMarkupsNode::AddControlPointWorld(vtkVector3d(posWorld));
      }
    }
  while (this->GetNumberOfControlPoints() > numberOfPoints)
    {
    this->RemoveNthControlPoint(this->GetNumberOfControlPoints() - 1);
    }
  this->EndModify(wasModified);
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::GetControlPointPositionsWorld(vtkPoints* points)
{
  if (!points)
    {
    return;
    }
  int numberOfControlPoints = this->GetNumberOfControlPoints();
  points->SetNumberOfPoints(numberOfControlPoints);
  double posWorld[3] = { 0.0 };
  for (int controlPointIndex = 0; controlPointIndex < numberOfControlPoints; controlPointIndex++)
    {
    this->GetNthControlPointPositionWorld(controlPointIndex, posWorld);
    points->SetPoint(controlPointIndex, posWorld);
    }
}

//---------------------------------------------------------------------------
bool vtkMRMLMarkupsNode::SetControlPointLabelsWorld(vtkStringArray* labels, vtkPoints* points, std::string separator /*=""*/)
{
  if (!labels || !points || labels->GetNumberOfValues() != points->GetNumberOfPoints())
    {
    vtkErrorMacro("vtkMRMLMarkupsNode::SetControlPointLabelsWorld failed: invalid inputs");
    return false;
    }

  int wasModified = this->StartModify();

  // Erase all previous labels
  int numberOfControlPoints = this->GetNumberOfControlPoints();
  for (int n = 0; n < numberOfControlPoints; n++)
    {
    this->SetNthControlPointLabel(n, "");
    }

  // Set each label at the closest control point
  vtkIdType numberOfLabels = labels->GetNumberOfValues();
  for (vtkIdType labelIndex = 0; labelIndex < numberOfLabels; ++labelIndex)
    {
    int markupIndex = this->GetClosestControlPointIndexToPositionWorld(points->GetPoint(labelIndex));
    if (markupIndex >= 0)
      {
      std::string oldLabel = this->GetNthControlPointLabel(markupIndex);
      if (oldLabel.empty())
        {
        this->SetNthControlPointLabel(markupIndex, labels->GetValue(labelIndex));
        }
      else
        {
        this->SetNthControlPointLabel(markupIndex, oldLabel + separator + labels->GetValue(labelIndex));
        }
      }
    }

  this->EndModify(wasModified);
  return true;
}

//---------------------------------------------------------------------------
int vtkMRMLMarkupsNode::GetNumberOfMeasurements()
{
  return static_cast<int>(this->Measurements.size());
}

//---------------------------------------------------------------------------
vtkMRMLMeasurement* vtkMRMLMarkupsNode::GetNthMeasurement(int id)
{
  if (id < 0 || id >= this->GetNumberOfMeasurements())
    {
    return nullptr;
    }
  return this->Measurements[id];
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthMeasurement(int id, vtkMRMLMeasurement* measurement)
{
  if (id < 0 || id > this->GetNumberOfMeasurements())
    {
    vtkErrorMacro("vtkMRMLMarkupsNode::SetNthMeasurement failed: id out of range");
    return;
    }
  if (id >= this->GetNumberOfMeasurements())
    {
    this->Measurements.push_back(measurement);
    }
  else
    {
    this->Measurements[id] = measurement;
    }
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::AddMeasurement(vtkMRMLMeasurement* measurement)
{
  this->Measurements.push_back(measurement);
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::SetNthMeasurement(int id,
  const std::string& name, double value, const std::string &units, const std::string description/*=""*/,
  vtkCodedEntry* quantityCode/*=nullptr*/, vtkCodedEntry* derivationCode/*=nullptr*/,
  vtkCodedEntry* unitsCode/*=nullptr*/, vtkCodedEntry* methodCode/*=nullptr*/)
{
  if (id < 0 || id > this->GetNumberOfMeasurements())
    {
    vtkErrorMacro("vtkMRMLMarkupsNode::SetNthMeasurement failed: id out of range");
    return;
    }
  vtkSmartPointer<vtkMRMLMeasurement> measurement;
  if (id >= this->GetNumberOfMeasurements())
    {
    measurement = vtkSmartPointer<vtkMRMLMeasurement>::New();
    this->Measurements.push_back(measurement);
    }
  else
    {
    measurement = this->Measurements[id];
    }
  measurement->SetName(name.c_str());
  measurement->SetValue(value);
  measurement->SetUnits(units.c_str());
  measurement->SetDescription(description.c_str());
  measurement->SetQuantityCode(quantityCode);
  measurement->SetDerivationCode(derivationCode);
  measurement->SetUnitsCode(unitsCode);
  measurement->SetMethodCode(methodCode);
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::RemoveNthMeasurement(int id)
{
  if (id < 0 || id >= this->GetNumberOfMeasurements())
    {
    vtkErrorMacro("vtkMRMLMarkupsNode::RemoveNthMeasurement failed: id out of range");
    }
  this->Measurements.erase(this->Measurements.begin() + id);
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::RemoveAllMeasurements()
{
  this->Measurements.clear();
}

//---------------------------------------------------------------------------
void vtkMRMLMarkupsNode::UpdateMeasurements()
{
  // child classes override this funciton to compute measurements
  this->RemoveAllMeasurements();
}
