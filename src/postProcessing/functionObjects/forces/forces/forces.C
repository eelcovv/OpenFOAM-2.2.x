/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2013 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "forces.H"
#include "volFields.H"
#include "dictionary.H"
#include "Time.H"
#include "wordReList.H"

#include "incompressible/singlePhaseTransportModel/singlePhaseTransportModel.H"
#include "incompressible/RAS/RASModel/RASModel.H"
#include "incompressible/LES/LESModel/LESModel.H"

#include "fluidThermo.H"
#include "compressible/RAS/RASModel/RASModel.H"
#include "compressible/LES/LESModel/LESModel.H"


// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
defineTypeNameAndDebug(forces, 0);
}


// * * * * * * * * * * * * Protected Member Functions  * * * * * * * * * * * //

void Foam::forces::writeFileHeader(const label i)
{
    file()
        << "# Time" << tab
        << "forces(pressure, viscous) moment(pressure, viscous)";

    if (localSystem_)
    {
        file()
            << tab
            << "local forces(pressure, viscous) "
            << "local moment(pressure, viscous)";
    }

    file()<< endl;
}


Foam::tmp<Foam::volSymmTensorField> Foam::forces::devRhoReff() const
{
    if (obr_.foundObject<compressible::RASModel>("RASProperties"))
    {
        const compressible::RASModel& ras
            = obr_.lookupObject<compressible::RASModel>("RASProperties");

        return ras.devRhoReff();
    }
    else if (obr_.foundObject<incompressible::RASModel>("RASProperties"))
    {
        const incompressible::RASModel& ras
            = obr_.lookupObject<incompressible::RASModel>("RASProperties");

        return rho()*ras.devReff();
    }
    else if (obr_.foundObject<compressible::LESModel>("LESProperties"))
    {
        const compressible::LESModel& les =
        obr_.lookupObject<compressible::LESModel>("LESProperties");

        return les.devRhoReff();
    }
    else if (obr_.foundObject<incompressible::LESModel>("LESProperties"))
    {
        const incompressible::LESModel& les
            = obr_.lookupObject<incompressible::LESModel>("LESProperties");

        return rho()*les.devReff();
    }
    else if (obr_.foundObject<fluidThermo>("thermophysicalProperties"))
    {
        const fluidThermo& thermo =
             obr_.lookupObject<fluidThermo>("thermophysicalProperties");

        const volVectorField& U = obr_.lookupObject<volVectorField>(UName_);

        return -thermo.mu()*dev(twoSymm(fvc::grad(U)));
    }
    else if
    (
        obr_.foundObject<singlePhaseTransportModel>("transportProperties")
    )
    {
        const singlePhaseTransportModel& laminarT =
            obr_.lookupObject<singlePhaseTransportModel>
            ("transportProperties");

        const volVectorField& U = obr_.lookupObject<volVectorField>(UName_);

        return -rho()*laminarT.nu()*dev(twoSymm(fvc::grad(U)));
    }
    else if (obr_.foundObject<dictionary>("transportProperties"))
    {
        const dictionary& transportProperties =
             obr_.lookupObject<dictionary>("transportProperties");

        dimensionedScalar nu(transportProperties.lookup("nu"));

        const volVectorField& U = obr_.lookupObject<volVectorField>(UName_);

        return -rho()*nu*dev(twoSymm(fvc::grad(U)));
    }
    else
    {
        FatalErrorIn("forces::devRhoReff()")
            << "No valid model for viscous stress calculation."
            << exit(FatalError);

        return volSymmTensorField::null();
    }
}


Foam::tmp<Foam::volScalarField> Foam::forces::rho() const
{
    if (rhoName_ == "rhoInf")
    {
        const fvMesh& mesh = refCast<const fvMesh>(obr_);

        return tmp<volScalarField>
        (
            new volScalarField
            (
                IOobject
                (
                    "rho",
                    mesh.time().timeName(),
                    mesh
                ),
                mesh,
                dimensionedScalar("rho", dimDensity, rhoRef_)
            )
        );
    }
    else
    {
        return(obr_.lookupObject<volScalarField>(rhoName_));
    }
}


Foam::scalar Foam::forces::rho(const volScalarField& p) const
{
    if (p.dimensions() == dimPressure)
    {
        return 1.0;
    }
    else
    {
        if (rhoName_ != "rhoInf")
        {
            FatalErrorIn("forces::rho(const volScalarField& p)")
                << "Dynamic pressure is expected but kinematic is provided."
                << exit(FatalError);
        }

        return rhoRef_;
    }
}


void Foam::forces::applyBins
(
    const label patchI,
    const vectorField fN,
    const vectorField Md,
    const vectorField fT
)
{
    if (nBin_ == 1)
    {
        force_[0][0] += sum(fN);
        force_[1][0] += sum(fT);
        moment_[0][0] += sum(Md ^ fN);
        moment_[1][0] += sum(Md ^ fT);
    }
    else
    {
        const fvMesh& mesh = refCast<const fvMesh>(obr_);
        const vectorField& Cf = mesh.C().boundaryField()[patchI];
        scalarField d((Cf & binDir_) - binMin_);

        forAll(d, i)
        {
            label binI = floor(d[i]/binDx_);
            force_[0][binI] += fN[i];
            force_[1][binI] += fT[i];
            moment_[0][binI] += Md[i]^fN[i];
            moment_[1][binI] += Md[i]^fT[i];
        }
    }
}


void Foam::forces::writeBins() const
{
    if (nBin_ == 1)
    {
        return;
    }

    autoPtr<writer<vector> > binWriterPtr(writer<vector>::New(binFormat_));
    coordSet axis("forces", "distance", binPoints_, mag(binPoints_));

    fileName forcesDir = baseTimeDir();
    mkDir(forcesDir);

    if (log_)
    {
        Info<< "    Writing bins to " << forcesDir << endl;
    }

    wordList fieldNames(IStringStream("(pressure viscous)")());

    List<Field<vector> > f(force_);
    List<Field<vector> > m(moment_);

    if (binCumulative_)
    {
        for (label i = 1; i < f[0].size(); i++)
        {
            f[0][i] += f[0][i-1];
            f[1][i] += f[1][i-1];
            m[0][i] += m[0][i-1];
            m[1][i] += m[1][i-1];
        }
    }

    OFstream osForce(forcesDir/"force_bins");
    binWriterPtr->write(axis, fieldNames, f, osForce);

    OFstream osMoment(forcesDir/"moment_bins");
    binWriterPtr->write(axis, fieldNames, m, osMoment);


    if (localSystem_)
    {
        List<Field<vector> > localForce(2);
        List<Field<vector> > localMoment(2);
        localForce[0] = coordSys_.localVector(force_[0]);
        localForce[1] = coordSys_.localVector(force_[1]);
        localMoment[0] = coordSys_.localVector(moment_[0]);
        localMoment[1] = coordSys_.localVector(moment_[1]);

        if (binCumulative_)
        {
            for (label i = 1; i < localForce[0].size(); i++)
            {
                localForce[0][i] += localForce[0][i-1];
                localForce[1][i] += localForce[1][i-1];
                localMoment[0][i] += localMoment[0][i-1];
                localMoment[1][i] += localMoment[1][i-1];
            }
        }

        OFstream osLocalForce(forcesDir/"force_local");
        binWriterPtr->write(axis, fieldNames, localForce, osLocalForce);

        OFstream osLocalMoment(forcesDir/"moment_local");
        binWriterPtr->write(axis, fieldNames, localMoment, osLocalMoment);
    }
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::forces::forces
(
    const word& name,
    const objectRegistry& obr,
    const dictionary& dict,
    const bool loadFromFiles
)
:
    functionObjectFile(obr, name, word(dict.lookup("type"))),
    name_(name),
    obr_(obr),
    active_(true),
    log_(false),
    force_(2),
    moment_(2),
    patchSet_(),
    pName_(word::null),
    UName_(word::null),
    rhoName_(word::null),
    directForceDensity_(false),
    fDName_(""),
    rhoRef_(VGREAT),
    pRef_(0),
    coordSys_(),
    localSystem_(false),
    nBin_(1),
    binDir_(vector::zero),
    binDx_(0.0),
    binMin_(GREAT),
    binPoints_(),
    binFormat_("undefined"),
    binCumulative_(true)
{
    // Check if the available mesh is an fvMesh otherise deactivate
    if (!isA<fvMesh>(obr_))
    {
        active_ = false;
        WarningIn
        (
            "Foam::forces::forces"
            "("
                "const word&, "
                "const objectRegistry&, "
                "const dictionary&, "
                "const bool"
            ")"
        )   << "No fvMesh available, deactivating."
            << endl;
    }

    read(dict);
}


Foam::forces::forces
(
    const word& name,
    const objectRegistry& obr,
    const labelHashSet& patchSet,
    const word& pName,
    const word& UName,
    const word& rhoName,
    const scalar rhoInf,
    const scalar pRef,
    const coordinateSystem& coordSys
)
:
    functionObjectFile(obr, name, typeName),
    name_(name),
    obr_(obr),
    active_(true),
    log_(false),
    force_(2),
    moment_(2),
    patchSet_(patchSet),
    pName_(pName),
    UName_(UName),
    rhoName_(rhoName),
    directForceDensity_(false),
    fDName_(""),
    rhoRef_(rhoInf),
    pRef_(pRef),
    coordSys_(coordSys),
    localSystem_(false),
    nBin_(1),
    binDir_(vector::zero),
    binDx_(0.0),
    binMin_(GREAT),
    binPoints_(),
    binFormat_("undefined"),
    binCumulative_(true)
{}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::forces::~forces()
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

void Foam::forces::read(const dictionary& dict)
{
    if (active_)
    {
        log_ = dict.lookupOrDefault<Switch>("log", false);
        directForceDensity_ = dict.lookupOrDefault("directForceDensity", false);

        const fvMesh& mesh = refCast<const fvMesh>(obr_);
        const polyBoundaryMesh& pbm = mesh.boundaryMesh();

        patchSet_ = pbm.patchSet(wordReList(dict.lookup("patches")));

        if (directForceDensity_)
        {
            // Optional entry for fDName
            fDName_ = dict.lookupOrDefault<word>("fDName", "fD");

            // Check whether fDName exists, if not deactivate forces
            if
            (
                !obr_.foundObject<volVectorField>(fDName_)
            )
            {
                active_ = false;
                WarningIn("void forces::read(const dictionary&)")
                    << "Could not find " << fDName_ << " in database." << nl
                    << "    De-activating forces."
                    << endl;
            }
        }
        else
        {
            // Optional entries U and p
            pName_ = dict.lookupOrDefault<word>("pName", "p");
            UName_ = dict.lookupOrDefault<word>("UName", "U");
            rhoName_ = dict.lookupOrDefault<word>("rhoName", "rho");

            // Check whether UName, pName and rhoName exists,
            // if not deactivate forces
            if
            (
                !obr_.foundObject<volVectorField>(UName_)
             || !obr_.foundObject<volScalarField>(pName_)
             || (
                    rhoName_ != "rhoInf"
                 && !obr_.foundObject<volScalarField>(rhoName_)
                )
            )
            {
                active_ = false;

                WarningIn("void forces::read(const dictionary&)")
                    << "Could not find " << UName_ << ", " << pName_;

                if (rhoName_ != "rhoInf")
                {
                    Info<< " or " << rhoName_;
                }

                Info<< " in database." << nl
                    << "    De-activating forces." << endl;
            }

            // Reference density needed for incompressible calculations
            rhoRef_ = readScalar(dict.lookup("rhoInf"));

            // Reference pressure, 0 by default
            pRef_ = dict.lookupOrDefault<scalar>("pRef", 0.0);
        }

        coordSys_.clear();

        // Centre of rotation for moment calculations
        // specified directly, from coordinate system, or implicitly (0 0 0)
        if (!dict.readIfPresent<point>("CofR", coordSys_.origin()))
        {
            coordSys_ = coordinateSystem(dict, obr_);
            localSystem_ = true;
        }

        if (dict.found("binData"))
        {
            const dictionary& binDict(dict.subDict("binData"));
            binDict.lookup("nBin") >> nBin_;

            if (nBin_ < 0)
            {
                FatalIOErrorIn
                (
                    "void Foam::forces::read(const dictionary&)", dict
                )   << "Number of bins (nBin) must be zero or greater"
                    << exit(FatalIOError);
            }
            else if ((nBin_ == 0) || (nBin_ == 1))
            {
                nBin_ = 1;
                force_[0].setSize(1);
                force_[1].setSize(1);
                moment_[0].setSize(1);
                moment_[1].setSize(1);
            }

            if (nBin_ > 1)
            {
                binDict.lookup("direction") >> binDir_;
                binDir_ /= mag(binDir_);

                binMin_ = GREAT;
                scalar binMax = -GREAT;
                forAllConstIter(labelHashSet, patchSet_, iter)
                {
                    label patchI = iter.key();
                    const polyPatch& pp = pbm[patchI];
                    scalarField d(pp.faceCentres() & binDir_);
                    binMin_ = min(min(d), binMin_);
                    binMax = max(max(d), binMax);
                }
                reduce(binMin_, minOp<scalar>());
                reduce(binMax, maxOp<scalar>());

                // slightly boost binMax so that region of interest is fully
                // within bounds
                binMax = 1.0001*(binMax - binMin_) + binMin_;

                binDx_ = (binMax - binMin_)/scalar(nBin_);

                // create the bin points used for writing
                binPoints_.setSize(nBin_);
                forAll(binPoints_, i)
                {
                    binPoints_[i] = (i + 0.5)*binDir_*binDx_;
                }

                binDict.lookup("format") >> binFormat_;

                binDict.lookup("cumulative") >> binCumulative_;

                // allocate storage for forces and moments
                forAll(force_, i)
                {
                    force_[i].setSize(nBin_);
                    moment_[i].setSize(nBin_);
                }
            }
        }

        if (nBin_ == 1)
        {
            // allocate storage for forces and moments
            force_[0].setSize(1);
            force_[1].setSize(1);
            moment_[0].setSize(1);
            moment_[1].setSize(1);
        }
    }
}


void Foam::forces::execute()
{
    // Do nothing - only valid on write
}


void Foam::forces::end()
{
    // Do nothing - only valid on write
}


void Foam::forces::write()
{
    if (!active_)
    {
        return;
    }

    calcForcesMoment();

    if (Pstream::master())
    {
        functionObjectFile::write();

        if (log_)
        {
            Info<< type() << " output:" << nl
                << "    forces(pressure,viscous)"
                << "(" << sum(force_[0]) << "," << sum(force_[1]) << ")" << nl
                << "    moment(pressure,viscous)"
                << "(" << sum(moment_[0]) << "," << sum(moment_[1]) << ")"
                << nl;
        }

        file() << obr_.time().value() << tab
            << "(" << sum(force_[0]) << "," << sum(force_[1]) << ") "
            << "(" << sum(moment_[0]) << "," << sum(moment_[1]) << ")"
            << endl;

        if (localSystem_)
        {
            vectorField localForceP(coordSys_.localVector(force_[0]));
            vectorField localForceV(coordSys_.localVector(force_[1]));
            vectorField localMomentP(coordSys_.localVector(moment_[0]));
            vectorField localMomentV(coordSys_.localVector(moment_[1]));

            file() << obr_.time().value() << tab
                << "(" << sum(localForceP) << "," << sum(localForceV) << ") "
                << "(" << sum(localMomentP) << "," << sum(localMomentV) << ")"
                << endl;
        }

        writeBins();

        if (log_)
        {
            Info<< endl;
        }
    }
}


void Foam::forces::calcForcesMoment()
{
    force_[0] = vector::zero;
    force_[1] = vector::zero;

    moment_[0] = vector::zero;
    moment_[1] = vector::zero;

    if (directForceDensity_)
    {
        const volVectorField& fD = obr_.lookupObject<volVectorField>(fDName_);

        const fvMesh& mesh = fD.mesh();

        const surfaceVectorField::GeometricBoundaryField& Sfb =
            mesh.Sf().boundaryField();

        forAllConstIter(labelHashSet, patchSet_, iter)
        {
            label patchI = iter.key();

            vectorField Md
            (
                mesh.C().boundaryField()[patchI] - coordSys_.origin()
            );

            scalarField sA(mag(Sfb[patchI]));

            // Normal force = surfaceUnitNormal*(surfaceNormal & forceDensity)
            vectorField fN
            (
                Sfb[patchI]/sA
               *(
                    Sfb[patchI] & fD.boundaryField()[patchI]
                )
            );

            // Tangential force (total force minus normal fN)
            vectorField fT(sA*fD.boundaryField()[patchI] - fN);

            applyBins(patchI, fN, Md, fT);
        }
    }
    else
    {
        const volVectorField& U = obr_.lookupObject<volVectorField>(UName_);
        const volScalarField& p = obr_.lookupObject<volScalarField>(pName_);

        const fvMesh& mesh = U.mesh();

        const surfaceVectorField::GeometricBoundaryField& Sfb =
            mesh.Sf().boundaryField();

        tmp<volSymmTensorField> tdevRhoReff = devRhoReff();
        const volSymmTensorField::GeometricBoundaryField& devRhoReffb
            = tdevRhoReff().boundaryField();

        // Scale pRef by density for incompressible simulations
        scalar pRef = pRef_/rho(p);

        forAllConstIter(labelHashSet, patchSet_, iter)
        {
            label patchI = iter.key();

            vectorField Md
            (
                mesh.C().boundaryField()[patchI] - coordSys_.origin()
            );

            vectorField pf
            (
                rho(p)*Sfb[patchI]*(p.boundaryField()[patchI] - pRef)
            );

            vectorField vf(Sfb[patchI] & devRhoReffb[patchI]);

            applyBins(patchI, pf, Md, vf);
        }
    }

    Pstream::listCombineGather(force_, plusEqOp<vectorField>());
    Pstream::listCombineGather(moment_, plusEqOp<vectorField>());
}


Foam::vector Foam::forces::forceEff() const
{
    return sum(force_[0]) + sum(force_[1]);
}


Foam::vector Foam::forces::momentEff() const
{
    return sum(moment_[0]) + sum(moment_[1]);
}


// ************************************************************************* //
